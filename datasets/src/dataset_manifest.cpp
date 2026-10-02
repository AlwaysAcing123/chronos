#include "chronos/datasets/dataset_manifest.hpp"

#include "chronos/datasets/json_fields.hpp"
#include "chronos/datasets/market_event_codec.hpp"
#include "chronos/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::datasets {
namespace {

namespace json = chronos::canonical_json;

json::Value decimal(std::uint64_t value) { return json::Value(std::to_string(value)); }
json::Value decimal(std::int64_t value) { return json::Value(std::to_string(value)); }
json::Value nanoseconds(Timestamp timestamp) { return decimal(timestamp.nanoseconds()); }

bool increment(std::uint64_t& counter) {
    if (counter == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    ++counter;
    return true;
}

std::uint64_t* type_counter(EventTypeCounts& counts, const MarketEvent& event) {
    return std::visit(
        [&counts](const auto& concrete) -> std::uint64_t* {
            using Event = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<Event, TradeEvent>) {
                return &counts.trade;
            } else if constexpr (std::is_same_v<Event, TopOfBookEvent>) {
                return &counts.top_of_book;
            } else if constexpr (std::is_same_v<Event, MarketStatusEvent>) {
                return &counts.market_status;
            } else {
                return &counts.instrument_definition;
            }
        },
        event
    );
}

json::Value manifest_body(const DatasetManifest& manifest) {
    const DatasetSummary& summary = manifest.summary;
    json::Object by_instrument;
    json::Array instruments;
    for (const auto& [instrument, count] : summary.by_instrument) {
        by_instrument.emplace_back(instrument, decimal(count));
        instruments.emplace_back(instrument);
    }
    const DatasetSource& source = manifest.source;
    const json::Value coverage = source.coverage
        ? json::Value(json::Object{
              {"end_ns", nanoseconds(source.coverage->end)},
              {"semantics", source.coverage->semantics},
              {"start_ns", nanoseconds(source.coverage->start)},
          })
        : json::Value(nullptr);
    return json::Value(json::Object{
        {"counts", json::Object{
            {"by_event_type", event_type_counts_json(summary.by_type)},
            {"by_instrument", std::move(by_instrument)},
            {"events", decimal(summary.events)},
        }},
        {"encoding", json::Object{
            {"name", kEventEncodingName},
            {"version", kEventEncodingVersion},
        }},
        {"files", json::Object{
            {"events", file_digest_json(kEventsFileName, manifest.events)},
            {"quality", file_digest_json(kQualityFileName, manifest.quality)},
        }},
        {"ingest_sequence", json::Object{
            {"first", decimal(summary.first_ingest_sequence)},
            {"last", decimal(summary.last_ingest_sequence)},
        }},
        {"instruments", std::move(instruments)},
        {"record", json::Object{
            {"name", kMarketEventRecordName},
            {"version", kMarketEventSchemaVersion},
        }},
        {"scales", json::Object{
            {"price_raw", decimal(Price::scale())},
            {"quantity_raw", decimal(Quantity::scale())},
            {"timestamps", kTimestampUnit},
        }},
        {"schema", json::Object{
            {"name", kDatasetSchemaName},
            {"version", kDatasetSchemaVersion},
        }},
        {"source", json::Object{
            {"coverage", coverage},
            {"description", source.description},
            {"normalizer", json::Object{
                {"name", source.normalizer},
                {"version", source.normalizer_version},
            }},
            {"provider", source.provider},
            {"provider_dataset", source.provider_dataset},
            {"provider_schema", source.provider_schema},
            {"recording_sha256", source.recording_sha256},
        }},
        {"stream", manifest.stream.value()},
        {"time_range", json::Object{
            {"event_timestamp_ns", json::Object{
                {"earliest", nanoseconds(summary.earliest_event_timestamp)},
                {"latest", nanoseconds(summary.latest_event_timestamp)},
            }},
            {"receive_timestamp_ns", json::Object{
                {"earliest", nanoseconds(summary.earliest_receive_timestamp)},
                {"latest", nanoseconds(summary.latest_receive_timestamp)},
            }},
        }},
    });
}

void require_nonempty(ObjectFields& fields, std::string_view key, const std::string& value) {
    if (fields.ok() && value.empty()) {
        fields.fail(DatasetErrorCode::InvalidValue, key, "must not be empty");
    }
}

}  // namespace

std::optional<DatasetError> DatasetSummaryBuilder::add(const ValidatedMarketEvent& event) {
    const MarketEventHeader& header = market_event_header(event.value());
    if (!summary_) {
        summary_ = DatasetSummary{
            .events = 0,
            .by_type = {},
            .by_instrument = {},
            .first_ingest_sequence = header.ingest_sequence,
            .last_ingest_sequence = header.ingest_sequence,
            .earliest_event_timestamp = header.event_timestamp,
            .latest_event_timestamp = header.event_timestamp,
            .earliest_receive_timestamp = header.receive_timestamp,
            .latest_receive_timestamp = header.receive_timestamp,
        };
    }
    // An error leaves the summary partly updated; the caller then abandons
    // the dataset.
    DatasetSummary& next = *summary_;
    if (!increment(next.events) ||
        !increment(*type_counter(next.by_type, event.value())) ||
        !increment(next.by_instrument[header.instrument.value()])) {
        return dataset_error(
            DatasetErrorCode::CounterOverflow,
            "counts",
            "an event count cannot be represented"
        );
    }
    next.last_ingest_sequence = header.ingest_sequence;
    next.earliest_event_timestamp = std::min(next.earliest_event_timestamp, header.event_timestamp);
    next.latest_event_timestamp = std::max(next.latest_event_timestamp, header.event_timestamp);
    next.earliest_receive_timestamp =
        std::min(next.earliest_receive_timestamp, header.receive_timestamp);
    next.latest_receive_timestamp = std::max(next.latest_receive_timestamp, header.receive_timestamp);
    return std::nullopt;
}

json::Value event_type_counts_json(const EventTypeCounts& counts) {
    return json::Value(json::Object{
        {"instrument_definition", decimal(counts.instrument_definition)},
        {"market_status", decimal(counts.market_status)},
        {"top_of_book", decimal(counts.top_of_book)},
        {"trade", decimal(counts.trade)},
    });
}

std::string dataset_identity(const DatasetManifest& manifest) {
    return Sha256::hex(json::serialize(manifest_body(manifest)));
}

json::Value manifest_json(const DatasetManifest& manifest) {
    json::Value body = manifest_body(manifest);
    json::Object members = *body.if_object();
    members.emplace_back("dataset_id", manifest.dataset_id);
    return json::Value(std::move(members));
}

std::string serialize_manifest(const DatasetManifest& manifest) {
    return canonical_document(manifest_json(manifest));
}

std::variant<DatasetManifest, DatasetError> parse_manifest(std::string_view bytes) {
    auto document = parse_canonical_document(bytes, kManifestFileName);
    if (auto* error = std::get_if<DatasetError>(&document)) {
        return std::move(*error);
    }
    const json::Value& root = std::get<json::Value>(document);
    ObjectFields fields(root, "manifest");
    // The schema decides every other field, so it is checked first.
    if (fields.ok()) {
        NestedFields schema(fields, "schema", {"name", "version"});
        schema.fields().constant("name", kDatasetSchemaName);
        schema.fields().constant("version", kDatasetSchemaVersion);
        schema.finish();
        if (!fields.ok()) {
            return *fields.error();
        }
    }
    fields.require_keys({
        "counts", "dataset_id", "encoding", "files", "ingest_sequence", "instruments",
        "record", "scales", "schema", "source", "stream", "time_range",
    });

    NestedFields encoding(fields, "encoding", {"name", "version"});
    encoding.fields().constant("name", kEventEncodingName);
    encoding.fields().constant("version", kEventEncodingVersion);
    encoding.finish();

    NestedFields record(fields, "record", {"name", "version"});
    record.fields().constant("name", kMarketEventRecordName);
    record.fields().constant("version", kMarketEventSchemaVersion);
    record.finish();

    NestedFields scales(fields, "scales", {"price_raw", "quantity_raw", "timestamps"});
    scales.fields().constant("price_raw", std::to_string(Price::scale()));
    scales.fields().constant("quantity_raw", std::to_string(Quantity::scale()));
    scales.fields().constant("timestamps", kTimestampUnit);
    scales.finish();

    std::string dataset_id = fields.sha256("dataset_id");
    std::string stream = fields.string("stream");
    require_nonempty(fields, "stream", stream);

    NestedFields source_fields(fields, "source", {
        "coverage", "description", "normalizer", "provider", "provider_dataset",
        "provider_schema", "recording_sha256",
    });
    ObjectFields& source = source_fields.fields();
    DatasetSource parsed_source{
        .provider = source.string("provider"),
        .provider_dataset = source.string("provider_dataset"),
        .provider_schema = source.string("provider_schema"),
        .description = source.string("description"),
        .recording_sha256 = source.sha256("recording_sha256"),
        .coverage = std::nullopt,
        .normalizer = {},
        .normalizer_version = {},
    };
    require_nonempty(source, "provider", parsed_source.provider);
    require_nonempty(source, "provider_dataset", parsed_source.provider_dataset);
    require_nonempty(source, "provider_schema", parsed_source.provider_schema);
    {
        NestedFields normalizer(source, "normalizer", {"name", "version"});
        parsed_source.normalizer = normalizer.fields().string("name");
        parsed_source.normalizer_version = normalizer.fields().string("version");
        require_nonempty(normalizer.fields(), "name", parsed_source.normalizer);
        require_nonempty(normalizer.fields(), "version", parsed_source.normalizer_version);
        normalizer.finish();
    }
    if (const json::Value* coverage = source.value("coverage"); coverage != nullptr && !coverage->is_null()) {
        NestedFields window(source, "coverage", {"end_ns", "semantics", "start_ns"});
        DatasetCoverage parsed_coverage{
            .start = window.fields().timestamp("start_ns"),
            .end = window.fields().timestamp("end_ns"),
            .semantics = window.fields().string("semantics"),
        };
        require_nonempty(window.fields(), "semantics", parsed_coverage.semantics);
        if (window.fields().ok() && parsed_coverage.end < parsed_coverage.start) {
            window.fields().fail(DatasetErrorCode::InvalidValue, "end_ns", "must not precede start_ns");
        }
        window.finish();
        parsed_source.coverage = std::move(parsed_coverage);
    }
    source_fields.finish();

    DatasetSummary summary;
    {
        NestedFields counts(fields, "counts", {"by_event_type", "by_instrument", "events"});
        summary.events = counts.fields().unsigned_integer("events");
        {
            NestedFields by_type(counts.fields(), "by_event_type", {
                "instrument_definition", "market_status", "top_of_book", "trade",
            });
            summary.by_type = EventTypeCounts{
                .trade = by_type.fields().unsigned_integer("trade"),
                .top_of_book = by_type.fields().unsigned_integer("top_of_book"),
                .market_status = by_type.fields().unsigned_integer("market_status"),
                .instrument_definition = by_type.fields().unsigned_integer("instrument_definition"),
            };
            by_type.finish();
        }
        if (const json::Object* by_instrument = counts.fields().object("by_instrument")) {
            for (const auto& [instrument, value] : *by_instrument) {
                const std::string* text = value.if_string();
                const auto count = text ? json::parse_canonical_unsigned(*text) : std::nullopt;
                if (instrument.empty() || !count || *count == 0) {
                    counts.fields().fail(
                        DatasetErrorCode::InvalidValue,
                        "by_instrument",
                        "must map non-empty instruments to positive canonical counts"
                    );
                    break;
                }
                summary.by_instrument.emplace(instrument, *count);
            }
        }
        counts.finish();
    }
    {
        NestedFields sequences(fields, "ingest_sequence", {"first", "last"});
        summary.first_ingest_sequence = sequences.fields().unsigned_integer("first");
        summary.last_ingest_sequence = sequences.fields().unsigned_integer("last");
        sequences.finish();
    }
    {
        NestedFields range(fields, "time_range", {"event_timestamp_ns", "receive_timestamp_ns"});
        {
            NestedFields event_range(range.fields(), "event_timestamp_ns", {"earliest", "latest"});
            summary.earliest_event_timestamp = event_range.fields().timestamp("earliest");
            summary.latest_event_timestamp = event_range.fields().timestamp("latest");
            event_range.finish();
        }
        {
            NestedFields receive_range(range.fields(), "receive_timestamp_ns", {"earliest", "latest"});
            summary.earliest_receive_timestamp = receive_range.fields().timestamp("earliest");
            summary.latest_receive_timestamp = receive_range.fields().timestamp("latest");
            receive_range.finish();
        }
        range.finish();
    }

    FileDigest events;
    FileDigest quality;
    {
        NestedFields files(fields, "files", {"events", "quality"});
        const auto file = [&files](std::string_view key, std::string_view name, FileDigest& digest) {
            digest = parse_file_digest(files.fields(), key, name);
            if (files.fields().ok() && digest.size_bytes == 0) {
                files.fields().fail(DatasetErrorCode::InvalidValue, std::string(key) + ".size_bytes", "must be positive");
            }
        };
        file("events", kEventsFileName, events);
        file("quality", kQualityFileName, quality);
        files.finish();
    }

    const std::vector<std::string> instruments = fields.string_array("instruments");
    if (!fields.ok()) {
        return *fields.error();
    }

    // An edit that did not recompute the ID is tampering, whatever else it
    // changed, so identity is checked before consistency.
    DatasetManifest manifest{
        .dataset_id = std::move(dataset_id),
        .stream = MarketDataStreamId(std::move(stream)),
        .source = std::move(parsed_source),
        .summary = std::move(summary),
        .events = std::move(events),
        .quality = std::move(quality),
    };
    if (dataset_identity(manifest) != manifest.dataset_id) {
        return dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "manifest.dataset_id",
            "does not match the manifest's content"
        );
    }

    // Consistency: the document must describe one possible dataset.
    const DatasetSummary& totals = manifest.summary;
    const auto inconsistent = [](std::string field, std::string message) {
        return dataset_error(DatasetErrorCode::SummaryMismatch, std::move(field), std::move(message));
    };
    for (std::size_t index = 0; index < instruments.size(); ++index) {
        if (instruments[index].empty() || (index > 0 && instruments[index - 1] >= instruments[index])) {
            return dataset_error(
                DatasetErrorCode::InvalidValue,
                "manifest.instruments",
                "must be non-empty, unique, and in byte order"
            );
        }
    }
    std::vector<std::string> counted;
    for (const auto& entry : totals.by_instrument) {
        counted.push_back(entry.first);
    }
    if (counted != instruments) {
        return inconsistent("manifest.instruments", "must list exactly the counted instruments");
    }
    if (totals.events == 0) {
        return inconsistent("manifest.counts.events", "a dataset holds at least one event");
    }
    std::uint64_t by_type_total = 0;
    std::uint64_t by_instrument_total = 0;
    bool representable = add_count(by_type_total, totals.by_type.trade) &&
        add_count(by_type_total, totals.by_type.top_of_book) &&
        add_count(by_type_total, totals.by_type.market_status) &&
        add_count(by_type_total, totals.by_type.instrument_definition);
    for (const auto& entry : totals.by_instrument) {
        representable = representable && add_count(by_instrument_total, entry.second);
    }
    if (!representable || by_type_total != totals.events) {
        return inconsistent("manifest.counts.by_event_type", "must add up to counts.events");
    }
    if (by_instrument_total != totals.events) {
        return inconsistent("manifest.counts.by_instrument", "must add up to counts.events");
    }
    if (totals.last_ingest_sequence < totals.first_ingest_sequence ||
        totals.last_ingest_sequence - totals.first_ingest_sequence != totals.events - 1) {
        return inconsistent(
            "manifest.ingest_sequence",
            "a contiguous sequence from first to last must hold counts.events events"
        );
    }
    if (totals.latest_event_timestamp < totals.earliest_event_timestamp ||
        totals.latest_receive_timestamp < totals.earliest_receive_timestamp) {
        return inconsistent("manifest.time_range", "latest must not precede earliest");
    }

    return manifest;
}

}  // namespace chronos::datasets

namespace chronos::datasets {

json::Value file_digest_json(std::string_view name, const FileDigest& digest) {
    return json::Value(json::Object{
        {"name", name},
        {"sha256", digest.sha256},
        {"size_bytes", std::to_string(digest.size_bytes)},
    });
}

FileDigest parse_file_digest(ObjectFields& parent, std::string_view key, std::string_view name) {
    NestedFields file(parent, key, {"name", "sha256", "size_bytes"});
    file.fields().constant("name", name);
    FileDigest digest{
        .sha256 = file.fields().sha256("sha256"),
        .size_bytes = file.fields().unsigned_integer("size_bytes"),
    };
    file.finish();
    return digest;
}

}  // namespace chronos::datasets
