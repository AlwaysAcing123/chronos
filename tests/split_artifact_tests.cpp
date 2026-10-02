#include "artifact_test_support.hpp"
#include "chronos/splits/chronological_split.hpp"
#include "chronos/splits/split_artifact.hpp"
#include "dataset_test_support.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace ds = chronos::datasets;
namespace lb = chronos::labels;
namespace sp = chronos::splits;
using chronos::InstrumentId;
using chronos::MarketDataStreamId;
using chronos::Timestamp;
using chronos::ValidatedMarketEvent;
using chronos::test::FeatureFixture;
using chronos::test::read_file;
using chronos::test::TemporaryDirectory;
using chronos::test::two_sided;
using chronos::test::write_file;
using sp::SplitExclusion;
using sp::SplitPeriod;

const MarketDataStreamId kStream{std::string(chronos::test::kDatasetStream)};

Timestamp at(std::int64_t nanoseconds) { return Timestamp::from_nanoseconds(nanoseconds); }

// A row whose label, when it has one, is known `delay` after its
// prediction.
sp::SplitInput input(std::string instrument, std::uint64_t sequence, std::int64_t prediction, std::optional<std::int64_t> delay) {
    return sp::SplitInput{
        .instrument = InstrumentId(std::move(instrument)),
        .ingest_sequence = sequence,
        .prediction_at = at(prediction),
        .label_usable = delay.has_value(),
        .label_available_at = delay ? std::optional(at(prediction + *delay)) : std::nullopt,
    };
}

// Rows at the given times, each with a label known `delay` later.
std::vector<sp::SplitInput> inputs_at(const std::vector<std::int64_t>& times, std::int64_t delay) {
    std::vector<sp::SplitInput> inputs;
    for (std::size_t index = 0; index < times.size(); ++index) {
        inputs.push_back(input("AAPL", index, times[index], delay));
    }
    return inputs;
}

std::vector<std::int64_t> every_ten(std::size_t count) {
    std::vector<std::int64_t> times;
    for (std::size_t index = 0; index < count; ++index) {
        times.push_back(static_cast<std::int64_t>(index) * 10);
    }
    return times;
}

struct Split {
    sp::SplitBoundaries boundaries;
    std::vector<sp::SplitRow> rows;
};

std::variant<Split, ds::DatasetError> split_all(
    const std::vector<sp::SplitInput>& inputs,
    const sp::SplitPercentages& percentages = sp::kDefaultSplitPercentages
) {
    Split split;
    auto result = sp::split_chronologically(kStream, inputs, percentages, [&](const sp::SplitRow& row) {
        split.rows.push_back(row);
        return std::optional<ds::DatasetError>{};
    });
    if (auto* error = std::get_if<ds::DatasetError>(&result)) {
        return *error;
    }
    split.boundaries = std::get<sp::SplitBoundaries>(result);
    return split;
}

Split split_of(const std::vector<sp::SplitInput>& inputs, const sp::SplitPercentages& percentages = sp::kDefaultSplitPercentages) {
    return chronos::test::succeeded(split_all(inputs, percentages), "splitting");
}

ds::DatasetErrorCode refusal(const std::vector<sp::SplitInput>& inputs, const sp::SplitPercentages& percentages = sp::kDefaultSplitPercentages) {
    return chronos::test::failed(split_all(inputs, percentages)).code;
}

// The guarantees every split must keep, asserted directly.
void require_invariants(const std::vector<sp::SplitRow>& rows, const sp::SplitBoundaries& boundaries) {
    REQUIRE(boundaries.validation_start > 0);
    REQUIRE(boundaries.validation_start < boundaries.test_start);
    REQUIRE(boundaries.test_start < rows.size());
    // Rows with equal prediction times are never divided.
    REQUIRE(rows[boundaries.validation_start - 1].prediction_at < rows[boundaries.validation_start].prediction_at);
    REQUIRE(rows[boundaries.test_start - 1].prediction_at < rows[boundaries.test_start].prediction_at);
    REQUIRE(boundaries.validation_start_at == rows[boundaries.validation_start].prediction_at);
    REQUIRE(boundaries.test_start_at == rows[boundaries.test_start].prediction_at);
    sp::ByPeriod<std::uint64_t> usable;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        REQUIRE(row.period == sp::period_of(index, boundaries));
        if (row.exclusion) {
            continue;
        }
        ++usable[row.period];
        REQUIRE(row.label_usable);
        REQUIRE(row.label_available_at.has_value());
        if (row.period == SplitPeriod::Train) {
            REQUIRE(*row.label_available_at < boundaries.validation_start_at);
            REQUIRE(row.prediction_at < boundaries.validation_start_at);
        }
        if (row.period == SplitPeriod::Validation) {
            REQUIRE(*row.label_available_at < boundaries.test_start_at);
            REQUIRE(row.prediction_at >= boundaries.validation_start_at);
            REQUIRE(row.prediction_at < boundaries.test_start_at);
        }
        if (row.period == SplitPeriod::Test) {
            REQUIRE(row.prediction_at >= boundaries.test_start_at);
        }
    }
    REQUIRE(usable.train > 0);
    REQUIRE(usable.validation > 0);
    REQUIRE(usable.test > 0);
}

void test_a_normal_split_is_contiguous_and_chronological() {
    const auto split = split_of(inputs_at(every_ten(10), 5));
    // floor(10 * 60 / 100) = 6 and floor(10 * 80 / 100) = 8.
    REQUIRE(split.boundaries == (sp::SplitBoundaries{
        .validation_start = 6, .test_start = 8, .validation_start_at = at(60), .test_start_at = at(80),
    }));
    for (std::size_t index = 0; index < split.rows.size(); ++index) {
        const auto& row = split.rows[index];
        REQUIRE(row.ingest_sequence == index);
        REQUIRE(!row.exclusion);
        REQUIRE(row.period == (index < 6 ? SplitPeriod::Train : index < 8 ? SplitPeriod::Validation : SplitPeriod::Test));
    }
    require_invariants(split.rows, split.boundaries);
    // Other whole percentages.
    const auto even = split_of(inputs_at(every_ten(10), 1), {.train = 34, .validation = 33, .test = 33});
    REQUIRE(even.boundaries.validation_start == 3);
    REQUIRE(even.boundaries.test_start == 6);
    const auto lopsided = split_of(inputs_at(every_ten(100), 1), {.train = 98, .validation = 1, .test = 1});
    REQUIRE(lopsided.boundaries.validation_start == 98);
    REQUIRE(lopsided.boundaries.test_start == 99);
}

void test_equal_prediction_times_are_never_divided() {
    // Rows 11, 12, and 13 share one time; the nominal boundary, row 12,
    // moves later past all of them.
    auto times = every_ten(20);
    times[12] = times[13] = times[11];
    const auto split = split_of(inputs_at(times, 1));
    REQUIRE(split.boundaries.validation_start == 14);
    REQUIRE(split.boundaries.test_start == 16);
    for (const std::size_t index : {11, 12, 13}) {
        REQUIRE(split.rows[index].period == SplitPeriod::Train);
    }
    require_invariants(split.rows, split.boundaries);
    // A tie that swallows validation's whole share leaves it empty, which
    // is refused rather than silently shrunk.
    auto wide = every_ten(10);
    for (std::size_t index = 5; index < 9; ++index) {
        wide[index] = 50;
    }
    REQUIRE(refusal(inputs_at(wide, 1)) == ds::DatasetErrorCode::EmptySplit);
    // So is one that runs to the end.
    auto tail = every_ten(10);
    for (std::size_t index = 7; index < 10; ++index) {
        tail[index] = 70;
    }
    REQUIRE(refusal(inputs_at(tail, 1)) == ds::DatasetErrorCode::EmptySplit);
}

void test_labels_crossing_a_boundary_are_purged() {
    // Labels known 15 after prediction: train's last row (50 -> 65) and
    // validation's last (70 -> 85) cross the next period's start.
    const auto split = split_of(inputs_at(every_ten(10), 15));
    REQUIRE(split.rows[4].exclusion == std::nullopt);
    REQUIRE(split.rows[5].exclusion == SplitExclusion::LabelCrossesValidationStart);
    REQUIRE(split.rows[5].period == SplitPeriod::Train);
    REQUIRE(split.rows[6].exclusion == std::nullopt);
    REQUIRE(split.rows[7].exclusion == SplitExclusion::LabelCrossesTestStart);
    REQUIRE(split.rows[7].period == SplitPeriod::Validation);
    // Test rows have no later period to protect.
    REQUIRE(split.rows[9].exclusion == std::nullopt);
    require_invariants(split.rows, split.boundaries);

    // A label known exactly when the next period begins is purged: the
    // comparison is strict.
    const auto exact = split_of(inputs_at(every_ten(10), 10));
    REQUIRE(*exact.rows[5].label_available_at == exact.boundaries.validation_start_at);
    REQUIRE(exact.rows[5].exclusion == SplitExclusion::LabelCrossesValidationStart);
    REQUIRE(*exact.rows[7].label_available_at == exact.boundaries.test_start_at);
    REQUIRE(exact.rows[7].exclusion == SplitExclusion::LabelCrossesTestStart);
    // One nanosecond earlier is usable.
    const auto earlier = split_of(inputs_at(every_ten(10), 9));
    REQUIRE(!earlier.rows[5].exclusion);
    REQUIRE(!earlier.rows[7].exclusion);
}

void test_unavailable_and_censored_labels_are_excluded_by_reason() {
    auto inputs = inputs_at(every_ten(10), 5);
    inputs[2] = input("AAPL", 2, 20, std::nullopt);
    // The test tail has no future observation.
    inputs[9] = input("AAPL", 9, 90, std::nullopt);
    // A label with a future observation but no value is unavailable too.
    inputs[3].label_usable = false;
    const auto split = split_of(inputs);
    REQUIRE(split.rows[2].exclusion == SplitExclusion::LabelUnavailable);
    REQUIRE(split.rows[3].exclusion == SplitExclusion::LabelUnavailable);
    REQUIRE(split.rows[3].label_available_at == at(35));
    REQUIRE(split.rows[9].exclusion == SplitExclusion::LabelUnavailable);
    REQUIRE(split.rows[9].period == SplitPeriod::Test);
    require_invariants(split.rows, split.boundaries);
    // A usable label without an availability time is a broken input.
    auto broken = inputs_at(every_ten(10), 5);
    broken[4].label_available_at.reset();
    const auto error = chronos::test::failed(split_all(broken));
    REQUIRE(error.code == ds::DatasetErrorCode::InvalidValue);
    REQUIRE(error.line == 5);
}

void test_empty_periods_and_splits_are_refused() {
    REQUIRE(refusal({}) == ds::DatasetErrorCode::EmptySplit);
    REQUIRE(refusal(inputs_at(every_ten(1), 1)) == ds::DatasetErrorCode::EmptySplit);
    REQUIRE(refusal(inputs_at(every_ten(2), 1)) == ds::DatasetErrorCode::EmptySplit);
    REQUIRE(split_of(inputs_at(every_ten(3), 1)).boundaries.test_start == 2);
    // Every training label crosses into validation.
    REQUIRE(refusal(inputs_at(every_ten(10), 100)) == ds::DatasetErrorCode::EmptySplit);
    // No test label has a value.
    auto censored = inputs_at(every_ten(10), 1);
    censored[8] = input("AAPL", 8, 80, std::nullopt);
    censored[9] = input("AAPL", 9, 90, std::nullopt);
    REQUIRE(refusal(censored) == ds::DatasetErrorCode::EmptySplit);
    // Prediction times must never decrease.
    auto backward = inputs_at(every_ten(10), 1);
    backward[4].prediction_at = at(25);
    const auto error = chronos::test::failed(split_all(backward));
    REQUIRE(error.code == ds::DatasetErrorCode::ReceiveTimeMovedBackward);
    REQUIRE(error.line == 5);
}

void test_percentages_are_whole_and_add_up_to_one_hundred() {
    const auto rows = inputs_at(every_ten(100), 1);
    for (const sp::SplitPercentages invalid : std::vector<sp::SplitPercentages>{
             {.train = 0, .validation = 50, .test = 50},
             {.train = 60, .validation = 0, .test = 40},
             {.train = 60, .validation = 40, .test = 0},
             {.train = 60, .validation = 20, .test = 21},
             {.train = 60, .validation = 20, .test = 19},
             {.train = 100, .validation = 0, .test = 0},
             {.train = 101, .validation = 0, .test = 0},
         }) {
        REQUIRE(sp::check_percentages(invalid)->code == ds::DatasetErrorCode::InvalidArgument);
        REQUIRE(refusal(rows, invalid) == ds::DatasetErrorCode::InvalidArgument);
    }
    REQUIRE(!sp::check_percentages(sp::kDefaultSplitPercentages));
    for (const SplitPeriod period : sp::kSplitPeriods) {
        REQUIRE(sp::period_from_name(sp::period_name(period)) == period);
    }
    for (const SplitExclusion exclusion : sp::kSplitExclusions) {
        REQUIRE(sp::exclusion_from_name(sp::exclusion_name(exclusion)) == exclusion);
    }
    REQUIRE(!sp::period_from_name("holdout"));
}

void test_random_splits_never_leak() {
    std::mt19937 generator(7);
    std::uniform_int_distribution<int> pick(0, 9);
    std::uniform_int_distribution<std::int64_t> step(0, 40);
    std::uniform_int_distribution<std::int64_t> delay(1, 300);
    std::size_t splits = 0;
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<sp::SplitInput> inputs;
        std::int64_t now = 0;
        const std::size_t count = 3 + static_cast<std::size_t>(pick(generator)) * 40;
        for (std::size_t index = 0; index < count; ++index) {
            now += pick(generator) < 3 ? 0 : step(generator);
            const std::string instrument = pick(generator) < 4 ? "AAPL" : "SPY";
            inputs.push_back(input(instrument, index, now, pick(generator) == 0 ? std::nullopt : std::optional(delay(generator))));
        }
        const std::uint64_t train = static_cast<std::uint64_t>(40 + pick(generator) * 4);
        auto result = split_all(inputs, {.train = train, .validation = 10, .test = 90 - train});
        if (auto* error = std::get_if<ds::DatasetError>(&result)) {
            REQUIRE(error->code == ds::DatasetErrorCode::EmptySplit);
            continue;
        }
        const auto& split = std::get<Split>(result);
        ++splits;
        require_invariants(split.rows, split.boundaries);
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            const auto& row = split.rows[index];
            REQUIRE(row.instrument == inputs[index].instrument);
            REQUIRE(row.ingest_sequence == inputs[index].ingest_sequence);
            REQUIRE(row.exclusion == sp::exclusion_of(row.period, inputs[index].label_usable, inputs[index].label_available_at, split.boundaries));
        }
    }
    REQUIRE(splits > 100);
}

// AAPL quotes every 500 ns until 4000 and then goes quiet; SPY quotes every
// 250 ns until 12000.
std::vector<ValidatedMarketEvent> fading_session() {
    std::vector<ValidatedMarketEvent> events;
    std::uint64_t sequence = 0;
    for (std::int64_t time = 1'000; time <= 12'000; time += 250) {
        if (time <= 4'000 && time % 500 == 0) {
            events.push_back(two_sided("AAPL", sequence++, time, time % 1'000 == 0 ? "100.00" : "100.01", "100.02"));
        }
        events.push_back(two_sided("SPY", sequence++, time, time % 750 == 0 ? "500.00" : "500.01", "500.02"));
    }
    return events;
}

class LabelFixture final {
public:
    explicit LabelFixture(const std::vector<ValidatedMarketEvent>& events, std::int64_t horizon_ns = 1'000)
        : features_(events), labels_(chronos::test::extracted_labels(features_.directory(), root_.path(), horizon_ns)) {}

    [[nodiscard]] const lb::PublishedLabelArtifact& artifact() const { return labels_; }
    [[nodiscard]] const std::filesystem::path& directory() const { return labels_.directory; }

private:
    FeatureFixture features_;
    TemporaryDirectory root_;
    lb::PublishedLabelArtifact labels_;
};

sp::PublishedSplitArtifact created(
    const std::filesystem::path& labels,
    const std::filesystem::path& root,
    const sp::SplitPercentages& percentages = sp::kDefaultSplitPercentages
) {
    return chronos::test::succeeded(sp::create_chronological_split(labels, root, percentages), "split creation");
}

std::vector<sp::SplitRow> rows_in(const std::filesystem::path& directory) {
    std::vector<sp::SplitRow> rows;
    const std::string bytes = read_file(directory / sp::kSplitRowsFileName);
    for (std::size_t start = 0; start < bytes.size();) {
        const std::size_t end = bytes.find('\n', start);
        rows.push_back(chronos::test::succeeded(sp::decode_split_row(std::string_view(bytes).substr(start, end - start)), "decoding"));
        start = end + 1;
    }
    return rows;
}

void test_an_artifact_assigns_every_label_row() {
    const LabelFixture labels(fading_session());
    const TemporaryDirectory root;
    const auto artifact = created(labels.directory(), root.path());
    REQUIRE(artifact.outcome == ds::PublicationOutcome::Published);
    const auto& manifest = artifact.manifest;
    REQUIRE(manifest.label_artifact_id == labels.artifact().manifest.artifact_id);
    REQUIRE(manifest.labels_sha256 == labels.artifact().manifest.labels.sha256);
    REQUIRE(manifest.feature_artifact_id == labels.artifact().manifest.feature_artifact_id);
    REQUIRE(manifest.dataset_id == labels.artifact().manifest.dataset_id);
    REQUIRE(manifest.horizon_ns == 1'000);
    REQUIRE(manifest.rows == labels.artifact().manifest.rows);

    // One row per label row, in order, with the same identity.
    const auto rows = rows_in(artifact.directory);
    auto reader = chronos::test::succeeded(lb::LabelArtifactReader::open(labels.directory()), "opening");
    for (const auto& row : rows) {
        auto next = reader->next();
        REQUIRE(std::holds_alternative<lb::FutureMidpointReturnRow>(next));
        const auto& label = std::get<lb::FutureMidpointReturnRow>(next);
        REQUIRE(row.instrument == label.instrument);
        REQUIRE(row.ingest_sequence == label.ingest_sequence);
        REQUIRE(row.prediction_at == label.prediction_at);
        REQUIRE(row.label_usable == std::holds_alternative<lb::FutureMidpointReturn>(label.label));
        REQUIRE(row.label_available_at == (label.future ? std::optional(label.future->available_at) : std::nullopt));
    }
    REQUIRE(std::holds_alternative<ds::EndOfRows>(reader->next()));
    require_invariants(rows, manifest.boundaries);

    // AAPL goes quiet early: its last labels have no future and are
    // excluded where they fall, in training.
    std::size_t quiet = 0;
    for (const auto& row : rows) {
        if (row.instrument.value() == "AAPL" && !row.label_usable) {
            ++quiet;
            REQUIRE(row.period == SplitPeriod::Train);
            REQUIRE(row.exclusion == SplitExclusion::LabelUnavailable);
        }
    }
    REQUIRE(quiet == 2);
    // The manifest's leakage bounds are the rows' own.
    REQUIRE(manifest.usable.train.max_label_available_at < manifest.boundaries.validation_start_at);
    REQUIRE(manifest.usable.validation.max_label_available_at < manifest.boundaries.test_start_at);
    std::uint64_t excluded = 0;
    for (const auto& [reason, count] : manifest.excluded) {
        excluded += count;
    }
    REQUIRE(manifest.usable.train.rows + manifest.usable.validation.rows + manifest.usable.test.rows + excluded == manifest.rows);
    REQUIRE(manifest.excluded.at("label_crosses_validation_start") > 0);
    REQUIRE(manifest.excluded.at("label_crosses_test_start") > 0);

    REQUIRE(chronos::test::succeeded(sp::verify_split_artifact(artifact.directory), "verification") == manifest);
    REQUIRE(!sp::reproduce_split_artifact(manifest, labels.directory()));
    REQUIRE(chronos::test::succeeded(sp::parse_split_manifest(read_file(artifact.directory / ds::kArtifactManifestFileName)), "parsing") ==
        manifest);
}

void test_creation_is_deterministic_and_never_overwrites() {
    const LabelFixture labels(fading_session());
    const TemporaryDirectory first;
    const TemporaryDirectory second;
    const auto one = created(labels.directory(), first.path());
    const auto two = created(labels.directory(), second.path());
    REQUIRE(one.manifest.artifact_id == two.manifest.artifact_id);
    for (const std::string_view file : {sp::kSplitRowsFileName, ds::kArtifactManifestFileName}) {
        REQUIRE(read_file(one.directory / file) == read_file(two.directory / file));
    }
    const auto permissions = std::filesystem::status(one.directory).permissions();
    REQUIRE((permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none);
    REQUIRE(created(labels.directory(), first.path()).outcome == ds::PublicationOutcome::AlreadyPresent);
    REQUIRE(chronos::test::entries_in(first.path()) == 1);

    // The percentages and the source are part of the identity.
    const auto other_shares = created(labels.directory(), first.path(), {.train = 50, .validation = 25, .test = 25});
    REQUIRE(other_shares.manifest.artifact_id != one.manifest.artifact_id);
    const LabelFixture other_labels(fading_session(), 1'500);
    const auto other = created(other_labels.directory(), first.path());
    REQUIRE(other.manifest.artifact_id != one.manifest.artifact_id);
    REQUIRE(other.manifest.horizon_ns == 1'500);
    REQUIRE(sp::reproduce_split_artifact(one.manifest, other_labels.directory())->code == ds::DatasetErrorCode::IdentityMismatch);
    // A manifest that names its source's ID with another checksum is not
    // that source's split.
    auto wrong_checksum = one.manifest;
    wrong_checksum.labels_sha256 = std::string(64, '0');
    wrong_checksum.artifact_id = sp::split_artifact_identity(wrong_checksum);
    REQUIRE(sp::reproduce_split_artifact(wrong_checksum, labels.directory())->code == ds::DatasetErrorCode::IdentityMismatch);
    REQUIRE(chronos::test::entries_in(first.path()) == 3);

    // Something else under the artifact's name is a collision, left alone.
    const TemporaryDirectory third;
    std::filesystem::create_directory(third.path() / one.manifest.artifact_id);
    REQUIRE(chronos::test::failed(sp::create_chronological_split(labels.directory(), third.path())).code ==
        ds::DatasetErrorCode::Collision);
    REQUIRE(chronos::test::entries_in(third.path() / one.manifest.artifact_id) == 0);

    // Refusals publish nothing.
    const TemporaryDirectory empty_root;
    REQUIRE(chronos::test::failed(sp::create_chronological_split(labels.directory(), empty_root.path(),
        {.train = 60, .validation = 20, .test = 21})).code == ds::DatasetErrorCode::InvalidArgument);
    const LabelFixture tiny({two_sided("AAPL", 0, 1'000, "1", "2"), two_sided("AAPL", 1, 3'000, "1", "2")});
    REQUIRE(chronos::test::failed(sp::create_chronological_split(tiny.directory(), empty_root.path())).code ==
        ds::DatasetErrorCode::EmptySplit);
    REQUIRE(chronos::test::failed(sp::create_chronological_split(labels.directory().parent_path() / std::string(64, 'e'),
        empty_root.path())).code == ds::DatasetErrorCode::NotFound);
    REQUIRE(chronos::test::entries_in(empty_root.path()) == 0);
}

void test_a_label_source_is_verified_completely() {
    // With a 900 ns horizon, AAPL's first label (prediction 1000) is known at
    // 2000, its next quote. Claiming 1900 instead keeps every per-row check
    // of the label artifact (later in ingest order, one horizon away, a
    // consistent elapsed time, the label its midpoints give), so only the
    // label verifier's second pass, which re-derives the future observation,
    // can see that a split built on it would be under-purged.
    const LabelFixture labels(fading_session(), 900);
    const std::string rows = read_file(labels.directory() / lb::kLabelRowsFileName);
    using chronos::test::replaced;
    const std::string earlier = replaced(
        replaced(rows, "\"elapsed_ns\":\"1000\"", "\"elapsed_ns\":\"900\""),
        "\"label_available_at_ns\":\"2000\"",
        "\"label_available_at_ns\":\"1900\""
    );
    const TemporaryDirectory forged_root;
    const auto forged = chronos::test::forge_label_artifact(forged_root.path(), labels.artifact().manifest, earlier);
    const TemporaryDirectory root;
    const auto refused = chronos::test::failed(sp::create_chronological_split(forged, root.path()));
    REQUIRE(refused.code == ds::DatasetErrorCode::InvalidValue);
    REQUIRE(refused.field == "future");
    REQUIRE(refused.line == 1);
    REQUIRE(chronos::test::entries_in(root.path()) == 0);
    // Reproduction against it is refused the same way.
    const auto genuine = created(labels.directory(), root.path());
    REQUIRE(sp::reproduce_split_artifact(genuine.manifest, forged)->field == "future");
}

std::filesystem::path forge_split_artifact(
    const std::filesystem::path& root,
    sp::SplitArtifactManifest manifest,
    std::string_view rows,
    bool keep_digest = false
) {
    if (!keep_digest) {
        manifest.splits = ds::FileDigest{.sha256 = chronos::Sha256::hex(rows), .size_bytes = rows.size()};
    }
    manifest.artifact_id = sp::split_artifact_identity(manifest);
    const std::filesystem::path directory = root / manifest.artifact_id;
    std::filesystem::create_directory(directory);
    write_file(directory / sp::kSplitRowsFileName, rows);
    write_file(directory / ds::kArtifactManifestFileName, sp::serialize_split_manifest(manifest));
    return directory;
}

void test_split_identity_and_invariants_are_checked_in_the_manifest() {
    const LabelFixture labels(fading_session());
    const TemporaryDirectory root;
    const auto artifact = created(labels.directory(), root.path());
    const std::string bytes = read_file(artifact.directory / ds::kArtifactManifestFileName);
    const auto error_of = [](const std::string& text) { return chronos::test::failed(sp::parse_split_manifest(text)).code; };
    using chronos::test::replaced;
    REQUIRE(error_of(replaced(bytes, "\"name\":\"chronos.split_artifact\",\"version\":\"1\"",
        "\"name\":\"chronos.split_artifact\",\"version\":\"2\"")) == ds::DatasetErrorCode::UnsupportedVersion);
    REQUIRE(error_of(replaced(bytes, "\"name\":\"chronological_purged\",\"version\":\"1\"",
        "\"name\":\"chronological_purged\",\"version\":\"2\"")) == ds::DatasetErrorCode::UnsupportedVersion);
    REQUIRE(error_of(replaced(bytes, "\"name\":\"chronos.splits.chronological_purged\",\"version\":\"1\"",
        "\"name\":\"chronos.splits.chronological_purged\",\"version\":\"9\"")) == ds::DatasetErrorCode::UnsupportedVersion);
    REQUIRE(error_of(replaced(bytes, "\"usable\":\"label_has_a_value\"", "\"usable\":\"anything\"")) ==
        ds::DatasetErrorCode::UnsupportedVersion);
    REQUIRE(error_of(replaced(bytes, "\"train\":\"60\"", "\"train\":\"070\"")) == ds::DatasetErrorCode::InvalidValue);
    REQUIRE(error_of(replaced(bytes, "\"train\":\"60\"", "\"train\":\"70\"")) == ds::DatasetErrorCode::IdentityMismatch);

    // Self-consistent IDs over broken invariants are still refused.
    const auto forged = [](sp::SplitArtifactManifest manifest) {
        manifest.artifact_id = sp::split_artifact_identity(manifest);
        return chronos::test::failed(sp::parse_split_manifest(sp::serialize_split_manifest(manifest))).code;
    };
    const auto& good = artifact.manifest;
    auto leaky_train = good;
    leaky_train.usable.train.max_label_available_at = good.boundaries.validation_start_at;
    REQUIRE(forged(leaky_train) == ds::DatasetErrorCode::LabelLeakage);
    auto leaky_validation = good;
    leaky_validation.usable.validation.max_label_available_at = good.boundaries.test_start_at;
    REQUIRE(forged(leaky_validation) == ds::DatasetErrorCode::LabelLeakage);
    auto bad_shares = good;
    bad_shares.percentages.test += 1;
    REQUIRE(forged(bad_shares) == ds::DatasetErrorCode::InvalidValue);
    auto overlapping = good;
    overlapping.periods.train.last_prediction_at = good.boundaries.validation_start_at;
    REQUIRE(forged(overlapping) == ds::DatasetErrorCode::SummaryMismatch);
    auto moved = good;
    moved.boundaries.validation_start += 1;
    REQUIRE(forged(moved) == ds::DatasetErrorCode::SummaryMismatch);
    auto empty_test = good;
    empty_test.usable.test.rows = 0;
    REQUIRE(forged(empty_test) == ds::DatasetErrorCode::SummaryMismatch);
    auto unaccounted = good;
    unaccounted.excluded["label_unavailable"] += 1;
    REQUIRE(forged(unaccounted) == ds::DatasetErrorCode::SummaryMismatch);
    auto unknown = good;
    unknown.excluded["holiday"] = 1;
    REQUIRE(forged(unknown) == ds::DatasetErrorCode::InvalidValue);
    for (const auto& change : std::vector<void (*)(sp::SplitArtifactManifest&)>{
             [](sp::SplitArtifactManifest& m) { m.label_artifact_id = std::string(64, '1'); },
             [](sp::SplitArtifactManifest& m) { m.labels_sha256 = std::string(64, '2'); },
             [](sp::SplitArtifactManifest& m) { m.feature_artifact_id = std::string(64, '3'); },
             [](sp::SplitArtifactManifest& m) { m.horizon_ns += 1; },
         }) {
        auto altered = good;
        change(altered);
        REQUIRE(sp::split_artifact_identity(altered) != good.artifact_id);
    }
}

void test_damaged_split_rows_fail_verification() {
    const LabelFixture labels(fading_session());
    const TemporaryDirectory root;
    const auto artifact = created(labels.directory(), root.path());
    const auto& manifest = artifact.manifest;
    const std::string rows = read_file(artifact.directory / sp::kSplitRowsFileName);
    const TemporaryDirectory copies;
    const auto error_of = [&](std::string_view forged_rows, const sp::SplitArtifactManifest& forged_manifest, bool keep = false) {
        return chronos::test::failed(sp::verify_split_artifact(forge_split_artifact(copies.path(), forged_manifest, forged_rows, keep)));
    };
    using chronos::test::replaced;
    const std::size_t second = rows.find('\n') + 1;
    // A dropped row shifts every later row across the manifest's boundary.
    const auto dropped = error_of(rows.substr(second), manifest);
    REQUIRE(dropped.code == ds::DatasetErrorCode::InvalidValue);
    REQUIRE(dropped.field == "period");
    REQUIRE(dropped.line == manifest.boundaries.validation_start);
    REQUIRE(error_of("{ " + rows.substr(1), manifest).code == ds::DatasetErrorCode::NotCanonical);
    REQUIRE(error_of("[" + rows.substr(1), manifest).code == ds::DatasetErrorCode::MalformedJson);
    REQUIRE(error_of(rows.substr(0, rows.size() - 1), manifest).code == ds::DatasetErrorCode::MissingFinalNewline);
    REQUIRE(error_of(replaced(rows, "\"instrument\":\"AAPL\"", "\"instrument\":\"AAPM\""), manifest, true).code ==
        ds::DatasetErrorCode::ChecksumMismatch);
    auto wrong_stream = manifest;
    wrong_stream.stream = MarketDataStreamId("elsewhere");
    REQUIRE(error_of(rows, wrong_stream).code == ds::DatasetErrorCode::StreamMismatch);

    // The first row: usable, in training.
    const std::string first = rows.substr(0, second - 1);
    REQUIRE(first.find("\"exclusion\":null") != std::string::npos);
    REQUIRE(first.find("\"split\":\"train\"") != std::string::npos);
    const auto with_first = [&](const std::string& altered) { return altered + rows.substr(second - 1); };
    REQUIRE(error_of(with_first(replaced(first, "\"split\":\"train\"", "\"split\":\"test\"")), manifest).field == "split");
    REQUIRE(error_of(with_first(replaced(first, "\"split\":\"train\"", "\"split\":\"excluded\"")), manifest).field == "split");
    REQUIRE(error_of(with_first(replaced(replaced(first, "\"period\":\"train\"", "\"period\":\"test\""),
        "\"split\":\"train\"", "\"split\":\"test\"")), manifest).field == "period");
    REQUIRE(error_of(with_first(replaced(replaced(first, "\"exclusion\":null", "\"exclusion\":\"label_unavailable\""),
        "\"split\":\"train\"", "\"split\":\"excluded\"")), manifest).field == "exclusion");
    REQUIRE(error_of(with_first(replaced(first, "\"label_usable\":true", "\"label_usable\":\"yes\"")), manifest).field ==
        "label_usable");

    // A label cannot be known sooner than one horizon after its prediction,
    // and a usable one must say when it is known.
    const std::size_t time = first.find("\"label_available_at_ns\":\"") + 25;
    const std::string available = first.substr(time, first.find('"', time) - time);
    const std::size_t predicted = first.find("\"prediction_at_ns\":\"") + 20;
    std::string too_soon = first;
    too_soon.replace(time, available.size(), first.substr(predicted, first.find('"', predicted) - predicted));
    REQUIRE(error_of(with_first(too_soon), manifest).field == "label_available_at_ns");
    const std::string no_time = replaced(
        replaced(
            replaced(first, "\"label_available_at_ns\":\"" + available + "\"", "\"label_available_at_ns\":null"),
            "\"exclusion\":null",
            "\"exclusion\":\"label_unavailable\""
        ),
        "\"split\":\"train\"",
        "\"split\":\"excluded\""
    );
    REQUIRE(error_of(with_first(no_time), manifest).field == "label_available_at_ns");

    // A row marked usable whose label is known after validation begins is
    // leakage, named as such.
    const std::string validation_start = std::to_string(manifest.boundaries.validation_start_at.nanoseconds());
    const std::size_t at = first.find("\"label_available_at_ns\":\"") + 25;
    std::string leaky = first;
    leaky.replace(at, first.find('"', at) - at, validation_start);
    const auto leak = error_of(with_first(leaky), manifest);
    REQUIRE(leak.code == ds::DatasetErrorCode::LabelLeakage);
    REQUIRE(leak.line == 1);

    // Rows consistent with forged boundaries that the percentages do not
    // give.
    auto shifted = manifest;
    shifted.percentages = {.train = 59, .validation = 21, .test = 20};
    const auto shift = error_of(rows, shifted);
    REQUIRE(shift.code == ds::DatasetErrorCode::InvalidValue);
    REQUIRE(shift.field == "manifest.boundaries");
    auto miscounted = manifest;
    miscounted.excluded["label_crosses_test_start"] -= 1;
    miscounted.excluded["label_unavailable"] += 1;
    REQUIRE(error_of(rows, miscounted).code == ds::DatasetErrorCode::SummaryMismatch);
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"a normal split is contiguous and chronological", test_a_normal_split_is_contiguous_and_chronological},
        {"equal prediction times are never divided", test_equal_prediction_times_are_never_divided},
        {"labels crossing a boundary are purged", test_labels_crossing_a_boundary_are_purged},
        {"unavailable and censored labels are excluded by reason", test_unavailable_and_censored_labels_are_excluded_by_reason},
        {"empty periods and splits are refused", test_empty_periods_and_splits_are_refused},
        {"percentages are whole and add up to one hundred", test_percentages_are_whole_and_add_up_to_one_hundred},
        {"random splits never leak", test_random_splits_never_leak},
        {"an artifact assigns every label row", test_an_artifact_assigns_every_label_row},
        {"creation is deterministic and never overwrites", test_creation_is_deterministic_and_never_overwrites},
        {"a label source is verified completely", test_a_label_source_is_verified_completely},
        {"split identity and invariants are checked in the manifest",
         test_split_identity_and_invariants_are_checked_in_the_manifest},
        {"damaged split rows fail verification", test_damaged_split_rows_fail_verification},
    });
}
