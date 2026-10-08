#include "chronos/strategy/strategy_run_artifact.hpp"

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/artifact_directory.hpp"
#include "chronos/datasets/dataset_reader.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/sha256.hpp"
#include "chronos/strategy/inference_schedule.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::strategy {
namespace {

namespace json = chronos::canonical_json;
using datasets::DatasetError;
using datasets::DatasetErrorCode;
using datasets::ObjectFields;

constexpr std::string_view kScope =
    "a deterministic paper simulation of one policy on a few minutes of one recorded session; it verifies the "
    "evaluation machinery and is not evidence that any model or policy would be profitable";

const json::Value& assumptions() {
    static const json::Value list(json::Array{
        "Executions use recorded top-of-book quotes only: a market order fills at the opposite displayed best price, "
        "up to its displayed quantity, with no depth, queue position, hidden liquidity, or market impact.",
        "An order arrives latency_ns after its signal and executes against the first top-of-book event of the "
        "instrument received at or after its arrival; whatever that observation is, its outcome is final.",
        "An order whose execution observation arrives while the instrument is halted or closed is rejected.",
        "An account takes each observation's displayed liquidity once; the strategy and the benchmarks are separate "
        "counterfactual accounts and do not share it.",
        "fee_per_fill is charged on every fill; it is an assumption, not a broker's fee schedule, and there are no "
        "borrow, financing, or exchange fees.",
        "Equity marks an open position at the best bid of the instrument's latest quote, which must be valid, "
        "unsuspect, and no older than maximum_quote_age_ns; a position that cannot be marked makes the valuation "
        "unavailable, never zero.",
        "Buy and hold buys target_quantity once, at the strategy's first decision, and holds; cash never trades.",
        "Orders still pending when the window ends expire unexecuted; open positions are not liquidated.",
    });
    return list;
}

const json::Value& definitions() {
    static const json::Value document(json::Object{
        {"decision",
         "a schedule row with a prediction, at least decision_interval_ns after the previous decision: up with "
         "confidence_ppb >= threshold_ppb targets target_quantity, down with confidence_ppb >= threshold_ppb targets "
         "flat, and anything else holds; an order is the target minus the position, and none is made while one is "
         "pending"},
        {"drawdown",
         "the largest fall of equity from its running peak over the valuation points, in money and as a percentage "
         "of that peak; points whose valuation is unavailable are counted and skipped"},
        {"return_percent", "net total PnL as a percentage of starting cash"},
        {"turnover", "the sum of every fill's notional, also as a percentage of starting cash"},
        {"valuation_points", "after each decision and each execution, and at the window's last applied event"},
    });
    return document;
}

json::Value environment_json() {
#if defined(__clang__)
    const std::string compiler = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    const std::string compiler = "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
        std::to_string(__GNUC_PATCHLEVEL__);
#else
    const std::string compiler = "unknown";
#endif
#if defined(__APPLE__)
    const std::string_view platform = "darwin";
#elif defined(__linux__)
    const std::string_view platform = "linux";
#else
    const std::string_view platform = "unknown";
#endif
#if defined(__aarch64__) || defined(__arm64__)
    const std::string_view architecture = "arm64";
#elif defined(__x86_64__)
    const std::string_view architecture = "x86_64";
#else
    const std::string_view architecture = "unknown";
#endif
    return json::Value(json::Object{
        {"architecture", architecture},
        {"arithmetic", "fixed-point integers with checked 128-bit intermediates; no floating point"},
        {"compiler", compiler},
        {"platform", platform},
    });
}

// What a run came from: the dataset and schedule, and the schedule's model,
// as that schedule's version names it.
json::Value source_json(const ScheduleManifest& source, const std::string& events_sha256) {
    json::Object members{
        {"dataset_id", source.dataset_id},
        {"events_sha256", events_sha256},
        {"feature_artifact_id", source.feature_artifact_id},
        {"instrument", source.instrument.value()},
        {"label_artifact_id", source.label_artifact_id},
        {"schedule_id", source.artifact_id},
        {"schedule_sha256", source.schedule.sha256},
        {"split_artifact_id", source.split_artifact_id},
        {"stream", source.stream.value()},
    };
    if (const auto* model = std::get_if<ExperimentScheduleModel>(&source.model)) {
        members.emplace_back("experiment_id", model->experiment_id);
        members.emplace_back("model", model->name);
        members.emplace_back("parameters_sha256", model->parameters_sha256);
        members.emplace_back(
            "training_labels_available_through_ns",
            std::to_string(model->training_labels_available_through.nanoseconds())
        );
    } else {
        const auto& registered = std::get<RegisteredScheduleModel>(source.model);
        members.emplace_back("family", registered.family);
        members.emplace_back("information_cutoff", json::Object{
            {"at_ns", std::to_string(registered.information_cutoff.nanoseconds())},
            {"basis", registered.cutoff_basis},
        });
        members.emplace_back("model_version_id", registered.model_version_id);
        members.emplace_back("payload_sha256", registered.payload_sha256);
    }
    return json::Value(std::move(members));
}

struct Simulated {
    std::string_view schema_version;
    json::Value source;
    json::Value window;
    StrategyResults results;
    std::uint64_t events{0};
    std::uint64_t after_window{0};
    std::uint64_t journal_entries{0};
};

std::variant<Simulated, DatasetError> simulate(
    const std::filesystem::path& catalog,
    const std::filesystem::path& schedule_directory,
    const StrategyConfiguration& configuration,
    const JournalSink& sink
) {
    auto opened_schedule = ScheduleReader::open(schedule_directory);
    if (auto* error = std::get_if<DatasetError>(&opened_schedule)) {
        return std::move(*error);
    }
    auto& schedule = std::get<std::unique_ptr<ScheduleReader>>(opened_schedule);
    const ScheduleManifest& source = schedule->manifest();
    auto opened_events = datasets::NormalizedMarketEventReader::open(catalog / source.dataset_id);
    if (auto* error = std::get_if<DatasetError>(&opened_events)) {
        return std::move(*error);
    }
    auto& events = std::get<std::unique_ptr<datasets::NormalizedMarketEventReader>>(opened_events);
    const datasets::DatasetManifest& dataset = events->manifest();
    if (dataset.dataset_id != source.dataset_id || dataset.stream != source.stream) {
        return datasets::dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "dataset",
            "the dataset is not the schedule's source"
        );
    }
    const bool registered = std::holds_alternative<RegisteredScheduleModel>(source.model);
    Simulated simulated{
        .schema_version = registered ? kRegisteredStrategyRunVersion : kExperimentStrategyRunVersion,
        .source = source_json(source, dataset.events.sha256),
        .window = json::Value(json::Object{
            {"end_at_ns", source.window.end_at ? json::Value(std::to_string(source.window.end_at->nanoseconds()))
                                               : json::Value(nullptr)},
            {"name", source.window.name},
            {"start_at_ns", std::to_string(source.window.start_at.nanoseconds())},
        }),
        .results = {},
        .events = 0,
        .after_window = 0,
        .journal_entries = 0,
    };
    StrategySimulation simulation(
        configuration,
        source.instrument,
        source.stream,
        source.window,
        "schedule:" + source.artifact_id,
        [&](const json::Value& entry) -> std::optional<DatasetError> {
            ++simulated.journal_entries;
            return sink(entry);
        }
    );
    const ScheduleRowSource rows = [&] { return schedule->next(); };
    bool ended = false;
    while (true) {
        MarketEventRead read = events->read_next();
        if (std::holds_alternative<EndOfMarketEvents>(read)) {
            break;
        }
        if (std::holds_alternative<MarketEventReadError>(read)) {
            return *events->error();
        }
        ++simulated.events;
        const auto& event = std::get<ValidatedMarketEvent>(read);
        // Events after the window are read only so the whole dataset verifies.
        if (!ended && simulation.accepts(event)) {
            if (auto error = simulation.apply(event, rows)) {
                return std::move(*error);
            }
        } else {
            ended = true;
            ++simulated.after_window;
        }
    }
    auto finished = simulation.finish();
    if (auto* error = std::get_if<DatasetError>(&finished)) {
        return std::move(*error);
    }
    simulated.results = std::get<StrategyResults>(std::move(finished));
    auto last = schedule->next();
    if (auto* error = std::get_if<DatasetError>(&last)) {
        return std::move(*error);
    }
    if (std::holds_alternative<ScheduleRow>(last)) {
        return datasets::dataset_error(
            DatasetErrorCode::CountMismatch,
            std::string(kScheduleRowsFileName),
            "the schedule has rows that no top-of-book event of the instrument in the window matches"
        );
    }
    return simulated;
}

json::Value manifest_body(
    const Simulated& simulated,
    const StrategyConfiguration& configuration,
    const datasets::FileDigest& journal,
    json::Value environment
) {
    return json::Value(json::Object{
        {"assumptions", assumptions()},
        {"configuration", configuration_json(configuration)},
        {"definitions", definitions()},
        {"environment", std::move(environment)},
        {"events", json::Object{
            {"after_window_unused", std::to_string(simulated.after_window)},
            {"read", std::to_string(simulated.events)},
        }},
        {"files", json::Object{{"journal", datasets::file_digest_json(kJournalFileName, journal)}}},
        {"journal", json::Object{{"entries", std::to_string(simulated.journal_entries)}}},
        {"record", datasets::name_version_json(kJournalRecordName, kJournalRecordVersion)},
        {"results", results_json(simulated.results, configuration)},
        {"runner", datasets::name_version_json(kRunnerName, kRunnerVersion)},
        {"schema", datasets::name_version_json(kStrategyRunSchemaName, simulated.schema_version)},
        {"scope", kScope},
        {"source", simulated.source},
        {"window", simulated.window},
    });
}

json::Value with_id(const json::Value& body, const std::string& id) {
    json::Object members = *body.if_object();
    members.emplace_back("artifact_id", id);
    return json::Value(std::move(members));
}

template <typename Decimal>
Decimal decimal_field(ObjectFields& fields, std::string_view key) {
    const std::string text = fields.string(key);
    if (!fields.ok()) {
        return Decimal{};
    }
    try {
        const Decimal value = Decimal::parse(text);
        if (value.to_string() == text) {
            return value;
        }
    } catch (const std::exception&) {
    }
    fields.fail(DatasetErrorCode::InvalidValue, key, "must be a canonical decimal");
    return Decimal{};
}

StrategyConfiguration parse_configuration(ObjectFields& parent) {
    StrategyConfiguration configuration;
    datasets::NestedFields nested(parent, "configuration", {"execution", "policy", "starting_cash"});
    ObjectFields& fields = nested.fields();
    configuration.starting_cash = decimal_field<Money>(fields, "starting_cash");
    {
        datasets::NestedFields execution(fields, "execution", {
            "fee_per_fill", "latency_ns", "maximum_order_notional", "maximum_quote_age_ns", "order_type",
            "time_in_force",
        });
        ObjectFields& entry = execution.fields();
        entry.constant("order_type", "market");
        entry.constant("time_in_force", "immediate_or_cancel");
        configuration.fee_per_fill = decimal_field<Money>(entry, "fee_per_fill");
        configuration.latency_ns = entry.signed_integer("latency_ns");
        configuration.maximum_order_notional = decimal_field<Money>(entry, "maximum_order_notional");
        configuration.maximum_quote_age_ns = entry.signed_integer("maximum_quote_age_ns");
        execution.finish();
    }
    {
        datasets::NestedFields policy(fields, "policy", {
            "decision_interval_ns", "name", "target_quantity", "threshold_ppb", "version",
        });
        ObjectFields& entry = policy.fields();
        entry.constant("name", kPolicyName);
        entry.constant("version", kPolicyVersion);
        configuration.decision_interval_ns = entry.signed_integer("decision_interval_ns");
        configuration.target_quantity = decimal_field<Quantity>(entry, "target_quantity");
        configuration.threshold_ppb = entry.unsigned_integer("threshold_ppb");
        policy.finish();
    }
    nested.finish();
    if (parent.ok()) {
        if (auto error = configuration_error(configuration)) {
            parent.fail(DatasetErrorCode::InvalidValue, "configuration", *error);
        }
    }
    return configuration;
}

constexpr std::string_view kEntryTypes[] = {"decision", "execution", "order", "order_expired", "valuation"};

std::optional<DatasetError> check_entry(std::string_view record, std::uint64_t line) {
    auto parsed = json::parse(record);
    if (auto* error = std::get_if<json::ParseError>(&parsed)) {
        return datasets::dataset_error(DatasetErrorCode::MalformedJson, "entry", error->message, line);
    }
    const json::Value& value = std::get<json::Value>(parsed);
    if (json::serialize(value) != record) {
        return datasets::dataset_error(DatasetErrorCode::NotCanonical, "entry", "record is not in canonical form", line);
    }
    const json::Object* object = value.if_object();
    const json::Value expected_record = datasets::name_version_json(kJournalRecordName, kJournalRecordVersion);
    const json::Value* type = nullptr;
    const json::Value* record_name = nullptr;
    if (object != nullptr) {
        for (const auto& [key, member] : *object) {
            if (key == "type") {
                type = &member;
            } else if (key == "record") {
                record_name = &member;
            }
        }
    }
    bool known = false;
    if (type != nullptr && type->if_string() != nullptr) {
        for (std::string_view name : kEntryTypes) {
            known = known || *type->if_string() == name;
        }
    }
    if (record_name == nullptr || *record_name != expected_record || !known) {
        return datasets::dataset_error(
            DatasetErrorCode::UnsupportedVersion,
            "entry",
            "is not a strategy journal entry of a known type",
            line
        );
    }
    return std::nullopt;
}

}  // namespace

std::variant<StrategyRunManifest, DatasetError> parse_strategy_run_manifest(std::string_view bytes) {
    auto document = datasets::parse_canonical_document(bytes, datasets::kArtifactManifestFileName);
    if (auto* error = std::get_if<DatasetError>(&document)) {
        return std::move(*error);
    }
    const json::Value& value = std::get<json::Value>(document);
    ObjectFields fields(value, "manifest");
    const std::string version = datasets::require_name_and_one_of_versions(
        fields, "schema", kStrategyRunSchemaName, {kExperimentStrategyRunVersion, kRegisteredStrategyRunVersion}
    );
    if (!fields.ok()) {
        return *fields.error();
    }
    fields.require_keys({
        "artifact_id", "assumptions", "configuration", "definitions", "environment", "events", "files", "journal",
        "record", "results", "runner", "schema", "scope", "source", "window",
    });
    datasets::require_name_version(fields, "record", kJournalRecordName, kJournalRecordVersion);
    datasets::require_name_version(fields, "runner", kRunnerName, kRunnerVersion);
    StrategyRunManifest manifest{
        .artifact_id = fields.sha256("artifact_id"),
        .schedule_id = {},
        .dataset_id = {},
        .configuration = parse_configuration(fields),
        .journal_entries = 0,
        .journal = {},
        .document = value,
    };
    for (std::string_view key : {"assumptions"}) {
        if (fields.ok() && fields.array(key) == nullptr) {
            fields.fail(DatasetErrorCode::InvalidValue, key, "must be an array");
        }
    }
    for (std::string_view key : {"definitions", "environment", "events", "results", "window"}) {
        if (fields.ok() && fields.object(key) == nullptr) {
            fields.fail(DatasetErrorCode::InvalidValue, key, "must be an object");
        }
    }
    if (const json::Value* source = fields.value("source"); source != nullptr && fields.ok()) {
        ObjectFields entry(*source, "manifest.source");
        if (version == kRegisteredStrategyRunVersion) {
            entry.require_keys({
                "dataset_id", "events_sha256", "family", "feature_artifact_id", "information_cutoff", "instrument",
                "label_artifact_id", "model_version_id", "payload_sha256", "schedule_id", "schedule_sha256",
                "split_artifact_id", "stream",
            });
            (void)entry.sha256("model_version_id");
        } else {
            entry.require_keys({
                "dataset_id", "events_sha256", "experiment_id", "feature_artifact_id", "instrument",
                "label_artifact_id", "model", "parameters_sha256", "schedule_id", "schedule_sha256",
                "split_artifact_id", "stream", "training_labels_available_through_ns",
            });
        }
        manifest.schedule_id = entry.sha256("schedule_id");
        manifest.dataset_id = entry.sha256("dataset_id");
        fields.adopt(entry.error());
    }
    {
        datasets::NestedFields journal(fields, "journal", {"entries"});
        manifest.journal_entries = journal.fields().unsigned_integer("entries");
        journal.finish();
    }
    {
        datasets::NestedFields files(fields, "files", {"journal"});
        manifest.journal = datasets::parse_file_digest(files.fields(), "journal", kJournalFileName);
        files.finish();
    }
    if (!fields.ok()) {
        return *fields.error();
    }
    json::Object body;
    for (const auto& member : *value.if_object()) {
        if (member.first != "artifact_id") {
            body.push_back(member);
        }
    }
    if (Sha256::hex(json::serialize(json::Value(std::move(body)))) != manifest.artifact_id) {
        return datasets::dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "manifest.artifact_id",
            "does not match the manifest's content"
        );
    }
    return manifest;
}

std::variant<PublishedStrategyRun, DatasetError> run_strategy(
    const std::filesystem::path& catalog,
    const std::filesystem::path& schedule_directory,
    const std::filesystem::path& output_root,
    const StrategyConfiguration& configuration
) {
    if (auto error = configuration_error(configuration)) {
        return datasets::dataset_error(DatasetErrorCode::InvalidArgument, "configuration", *error);
    }
    std::optional<Simulated> simulated;
    return datasets::stage_and_publish<StrategyRunManifest>(
        output_root,
        kStrategyRunLayout,
        [&](datasets::StagedFile& journal) -> std::optional<DatasetError> {
            std::uint64_t line = 0;
            auto result = simulate(catalog, schedule_directory, configuration, [&](const json::Value& entry) {
                return datasets::write_record(journal, json::serialize(entry), "journal entry", ++line);
            });
            if (auto* error = std::get_if<DatasetError>(&result)) {
                return std::move(*error);
            }
            simulated = std::get<Simulated>(std::move(result));
            return std::nullopt;
        },
        [&](datasets::FileDigest digest) -> std::variant<StrategyRunManifest, DatasetError> {
            const json::Value body = manifest_body(*simulated, configuration, digest, environment_json());
            const std::string id = Sha256::hex(json::serialize(body));
            // Never publish what the reader would refuse.
            return parse_strategy_run_manifest(datasets::canonical_document(with_id(body, id)));
        },
        [](const StrategyRunManifest& manifest) { return datasets::canonical_document(manifest.document); },
        verify_strategy_run
    );
}

std::variant<StrategyRunManifest, DatasetError> verify_strategy_run(const std::filesystem::path& directory) {
    auto opened = datasets::open_artifact_manifest(
        directory, kStrategyRunLayout, parse_strategy_run_manifest, &StrategyRunManifest::journal
    );
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    auto& [manifest, path] = std::get<datasets::OpenedArtifact<StrategyRunManifest>>(opened);
    auto file = datasets::DerivedArtifactRows::open(path, kStrategyRunLayout, manifest.journal_entries, manifest.journal);
    if (auto* error = std::get_if<DatasetError>(&file)) {
        return std::move(*error);
    }
    auto& rows = std::get<std::unique_ptr<datasets::DerivedArtifactRows>>(file);
    while (true) {
        auto next = rows->next();
        if (auto* error = std::get_if<DatasetError>(&next)) {
            return std::move(*error);
        }
        if (std::holds_alternative<datasets::EndOfRows>(next)) {
            return std::move(manifest);
        }
        if (auto error = check_entry(std::get<std::string_view>(next), rows->line_number())) {
            return std::move(*error);
        }
    }
}

std::optional<DatasetError> reproduce_strategy_run(
    const StrategyRunManifest& manifest,
    const std::filesystem::path& catalog,
    const std::filesystem::path& schedules_root
) {
    Sha256 hash;
    std::uint64_t size = 0;
    auto result = simulate(catalog, schedules_root / manifest.schedule_id, manifest.configuration, [&](const json::Value& entry) {
        const std::string line = json::serialize(entry) + "\n";
        hash.update(line);
        size += line.size();
        return std::optional<DatasetError>{};
    });
    if (auto* error = std::get_if<DatasetError>(&result)) {
        return std::move(*error);
    }
    const json::Value* environment = nullptr;
    for (const auto& [key, member] : *manifest.document.if_object()) {
        if (key == "environment") {
            environment = &member;
        }
    }
    const json::Value body = manifest_body(
        std::get<Simulated>(result),
        manifest.configuration,
        datasets::FileDigest{.sha256 = hash.hex_digest(), .size_bytes = size},
        environment ? *environment : json::Value(nullptr)
    );
    if (Sha256::hex(json::serialize(body)) != manifest.artifact_id) {
        return datasets::dataset_error(
            DatasetErrorCode::ChecksumMismatch,
            std::string(kJournalFileName),
            "running the schedule again gives a different journal or different results"
        );
    }
    return std::nullopt;
}

std::variant<std::vector<StrategyRunEntry>, DatasetError> list_strategy_runs(const std::filesystem::path& root) {
    return datasets::list_derived_artifacts(
        root, "output", kStrategyRunLayout, parse_strategy_run_manifest, &StrategyRunManifest::journal
    );
}

}  // namespace chronos::strategy
