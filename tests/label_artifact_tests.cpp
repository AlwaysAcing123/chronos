#include "artifact_test_support.hpp"
#include "chronos/labels/future_midpoint_return.hpp"
#include "chronos/labels/label_artifact.hpp"
#include "dataset_test_support.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
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
namespace ft = chronos::features;
namespace lb = chronos::labels;
using chronos::InstrumentId;
using chronos::MarketDataStreamId;
using chronos::Price;
using chronos::Timestamp;
using chronos::TradingStatus;
using chronos::ValidatedMarketEvent;
using chronos::test::FeatureFixture;
using chronos::test::header;
using chronos::test::level;
using chronos::test::quote;
using chronos::test::read_file;
using chronos::test::status;
using chronos::test::TemporaryDirectory;
using chronos::test::two_sided;
using chronos::test::write_file;
using lb::Direction;
using lb::LabelUnavailability;

constexpr std::int64_t kMaximum = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kSecond = 1'000'000'000;
const MarketDataStreamId kStream{std::string(chronos::test::kDatasetStream)};

ft::Feature<Price> mid(std::string_view price) { return Price::parse(price); }
ft::Feature<Price> raw_mid(std::int64_t raw) { return Price::from_raw(raw); }

lb::LabelObservation observation(std::string instrument, std::uint64_t sequence, std::int64_t at, ft::Feature<Price> midpoint) {
    return lb::LabelObservation{
        .instrument = InstrumentId(std::move(instrument)),
        .ingest_sequence = sequence,
        .available_at = Timestamp::from_nanoseconds(at),
        .midpoint = midpoint,
    };
}

std::variant<std::vector<lb::FutureMidpointReturnRow>, ds::DatasetError> label_all(
    const std::vector<lb::LabelObservation>& observations,
    std::int64_t horizon_ns
) {
    std::vector<lb::FutureMidpointReturnRow> rows;
    auto error = lb::label_future_midpoint_returns(kStream, observations, horizon_ns, [&](const auto& row) {
        rows.push_back(row);
        return std::optional<ds::DatasetError>{};
    });
    if (error) {
        return *error;
    }
    return rows;
}

std::vector<lb::FutureMidpointReturnRow> labelled(const std::vector<lb::LabelObservation>& observations, std::int64_t horizon_ns) {
    return chronos::test::succeeded(label_all(observations, horizon_ns), "labelling");
}

lb::FutureMidpointReturn value_of(const lb::Label& label) {
    REQUIRE(std::holds_alternative<lb::FutureMidpointReturn>(label));
    return std::get<lb::FutureMidpointReturn>(label);
}

LabelUnavailability reason_of(const lb::Label& label) {
    REQUIRE(std::holds_alternative<LabelUnavailability>(label));
    return std::get<LabelUnavailability>(label);
}

std::uint64_t future_sequence(const lb::FutureMidpointReturnRow& row) {
    REQUIRE(row.future.has_value());
    return row.future->ingest_sequence;
}

void test_returns_are_exact_and_round_half_away_from_zero() {
    // (101 - 100) / 100 = 0.01, (99 - 100) / 100 = -0.01, and no change is
    // flat.
    REQUIRE(value_of(lb::midpoint_return(Price::parse("100"), Price::parse("101"))) ==
        (lb::FutureMidpointReturn{.value = lb::Return::parse("0.01"), .direction = Direction::Up}));
    REQUIRE(value_of(lb::midpoint_return(Price::parse("100"), Price::parse("99"))) ==
        (lb::FutureMidpointReturn{.value = lb::Return::parse("-0.01"), .direction = Direction::Down}));
    REQUIRE(value_of(lb::midpoint_return(Price::parse("100"), Price::parse("100"))) ==
        (lb::FutureMidpointReturn{.value = lb::Return::parse("0"), .direction = Direction::Flat}));
    // 1e-9 / 2 is exactly half the return's unit: away from zero both ways.
    REQUIRE(value_of(lb::midpoint_return(Price::parse("2"), Price::from_raw(2'000'000'001))).value.raw() == 1);
    REQUIRE(value_of(lb::midpoint_return(Price::parse("2"), Price::from_raw(1'999'999'999))).value.raw() == -1);
    // Just under half rounds toward zero, but the direction is the exact
    // sign: no deadband.
    const auto tiny = value_of(lb::midpoint_return(Price::parse("3"), Price::from_raw(3'000'000'001)));
    REQUIRE(tiny.value.raw() == 0);
    REQUIRE(tiny.direction == Direction::Up);
    const auto tiny_down = value_of(lb::midpoint_return(Price::parse("3"), Price::from_raw(2'999'999'999)));
    REQUIRE(tiny_down.value.raw() == 0);
    REQUIRE(tiny_down.direction == Direction::Down);
    // 2/3 of the unit rounds to 1 either way.
    REQUIRE(value_of(lb::midpoint_return(Price::parse("1.5"), Price::from_raw(1'500'000'001))).value.raw() == 1);
    REQUIRE(value_of(lb::midpoint_return(Price::parse("1.5"), Price::from_raw(1'499'999'999))).value.raw() == -1);

    // Overflow and nonpositive midpoints are unavailable, never wrapped or
    // divided by.
    REQUIRE(reason_of(lb::midpoint_return(Price::from_raw(1), Price::from_raw(kMaximum))) ==
        LabelUnavailability::ReturnOverflow);
    REQUIRE(reason_of(lb::midpoint_return(Price::from_raw(1), Price::from_raw(std::numeric_limits<std::int64_t>::min()))) ==
        LabelUnavailability::ReturnOverflow);
    // The largest representable return is fine.
    REQUIRE(value_of(lb::midpoint_return(Price::from_raw(1'000'000'000), Price::from_raw(kMaximum))).value.raw() ==
        kMaximum - 1'000'000'000);
    REQUIRE(reason_of(lb::midpoint_return(Price::from_raw(0), Price::parse("1"))) ==
        LabelUnavailability::NonpositiveCurrentMidpoint);
    REQUIRE(reason_of(lb::midpoint_return(Price::from_raw(-5), Price::parse("1"))) ==
        LabelUnavailability::NonpositiveCurrentMidpoint);
}

void test_the_first_observation_at_or_after_the_horizon_is_selected() {
    // AAPL quotes every 400 ms, SPY every 100 ms, interleaved.
    std::vector<lb::LabelObservation> observations;
    std::uint64_t sequence = 0;
    for (std::int64_t at = 0; at <= 3 * kSecond; at += kSecond / 10) {
        if (at % (4 * kSecond / 10) == 0) {
            observations.push_back(observation("AAPL", sequence++, at, mid("100")));
        }
        observations.push_back(observation("SPY", sequence++, at, mid("500")));
    }
    const auto rows = labelled(observations, kSecond);
    REQUIRE(rows.size() == observations.size());
    for (std::size_t index = 0; index < rows.size(); ++index) {
        // Rows keep the feature rows' identity and order.
        REQUIRE(rows[index].instrument == observations[index].instrument);
        REQUIRE(rows[index].ingest_sequence == observations[index].ingest_sequence);
        REQUIRE(rows[index].prediction_at == observations[index].available_at);
        REQUIRE(rows[index].horizon_ns == kSecond);
    }
    const auto row_at = [&](std::string_view instrument, std::int64_t at) {
        for (const auto& row : rows) {
            if (row.instrument.value() == instrument && row.prediction_at.nanoseconds() == at) {
                return row;
            }
        }
        throw chronos::test::TestFailure("no row");
    };
    const auto observed_at = [&](const lb::FutureMidpointReturnRow& row) {
        REQUIRE(row.future.has_value());
        return row.future->available_at.nanoseconds();
    };
    // SPY at 0 s: a quote exactly one second later qualifies.
    REQUIRE(observed_at(row_at("SPY", 0)) == kSecond);
    REQUIRE(row_at("SPY", 0).future->elapsed_ns == kSecond);
    // AAPL at 0 s: no quote at 1.0 s; the first one after is at 1.2 s.
    REQUIRE(observed_at(row_at("AAPL", 0)) == 12 * kSecond / 10);
    REQUIRE(row_at("AAPL", 0).future->elapsed_ns == 12 * kSecond / 10);
    // The SPY quote at 1.0 s comes after AAPL's at 0.8 s in ingest order but
    // belongs to another instrument, so it is never AAPL's future.
    REQUIRE(observed_at(row_at("AAPL", 8 * kSecond / 10)) == 2 * kSecond);
    // The tail of each instrument is censored.
    REQUIRE(reason_of(row_at("SPY", 25 * kSecond / 10).label) == LabelUnavailability::NoFutureObservation);
    REQUIRE(!row_at("SPY", 25 * kSecond / 10).future);
    REQUIRE(observed_at(row_at("SPY", 2 * kSecond)) == 3 * kSecond);
    REQUIRE(reason_of(row_at("AAPL", 24 * kSecond / 10).label) == LabelUnavailability::NoFutureObservation);

    // Equal timestamps: the first row at the qualifying time is selected.
    const auto ties = labelled({
        observation("AAPL", 10, 0, mid("100")),
        observation("AAPL", 11, 1'000, mid("101")),
        observation("AAPL", 12, 1'000, mid("102")),
    }, 1'000);
    REQUIRE(future_sequence(ties[0]) == 11);
    REQUIRE(value_of(ties[0].label).value == lb::Return::parse("0.01"));
}

void test_an_unavailable_first_future_midpoint_is_never_skipped() {
    // The first quote at or after the horizon is one-sided. The next has a
    // midpoint, but using it would let future market state choose the
    // horizon.
    const auto rows = labelled({
        observation("AAPL", 0, 0, mid("100")),
        observation("AAPL", 1, 1'000, ft::Unavailability::MissingAsk),
        observation("AAPL", 2, 1'001, mid("105")),
    }, 1'000);
    REQUIRE(reason_of(rows[0].label) == LabelUnavailability::FutureMidpointUnavailable);
    REQUIRE(future_sequence(rows[0]) == 1);
    REQUIRE(rows[0].future->midpoint == ft::Feature<Price>(ft::Unavailability::MissingAsk));
    REQUIRE(rows[0].future->available_at == Timestamp::from_nanoseconds(1'000));
}

void test_unavailable_labels_have_one_reason_each_in_order() {
    const auto rows = labelled({
        // The current midpoint is unavailable; the future observation is
        // still recorded, since selecting it depends only on time.
        observation("AAPL", 0, 0, ft::Unavailability::TradingHalted),
        observation("AAPL", 1, 1'000, mid("100")),
        // A nonpositive midpoint cannot be divided by.
        observation("BAD", 2, 0, raw_mid(0)),
        observation("BAD", 3, 1'000, mid("1")),
        // The return does not fit the fixed-point range.
        observation("HUGE", 4, 0, raw_mid(1)),
        observation("HUGE", 5, 1'000, raw_mid(kMaximum)),
        // The horizon's end is not representable.
        observation("LATE", 6, kMaximum - 999, mid("10")),
        observation("LATE", 7, kMaximum, mid("11")),
    }, 1'000);
    REQUIRE(reason_of(rows[0].label) == LabelUnavailability::CurrentMidpointUnavailable);
    REQUIRE(future_sequence(rows[0]) == 1);
    REQUIRE(reason_of(rows[1].label) == LabelUnavailability::NoFutureObservation);
    REQUIRE(reason_of(rows[2].label) == LabelUnavailability::NonpositiveCurrentMidpoint);
    REQUIRE(reason_of(rows[4].label) == LabelUnavailability::ReturnOverflow);
    REQUIRE(future_sequence(rows[4]) == 5);
    // kMaximum - 999 + 1000 overflows: no future can exist.
    REQUIRE(reason_of(rows[6].label) == LabelUnavailability::TimestampOverflow);
    REQUIRE(!rows[6].future);
    REQUIRE(reason_of(rows[7].label) == LabelUnavailability::TimestampOverflow);
    // One below overflow still finds its future.
    const auto edge = labelled({
        observation("LATE", 0, kMaximum - 1'000, mid("10")),
        observation("LATE", 1, kMaximum, mid("11")),
    }, 1'000);
    REQUIRE(value_of(edge[0].label).direction == Direction::Up);
    REQUIRE(edge[0].future->elapsed_ns == 1'000);
    // A current-side reason comes before a future-side one.
    const auto both = labelled({
        observation("AAPL", 0, 0, ft::Unavailability::SuspectQuote),
        observation("AAPL", 1, 1'000, ft::Unavailability::SuspectQuote),
    }, 1'000);
    REQUIRE(reason_of(both[0].label) == LabelUnavailability::CurrentMidpointUnavailable);
    // Every reason has a stable name.
    for (const LabelUnavailability reason : lb::kLabelUnavailabilities) {
        REQUIRE(lb::label_unavailability_from_name(lb::label_unavailability_name(reason)) == reason);
    }
    REQUIRE(!lb::label_unavailability_from_name("stale"));
    REQUIRE(lb::direction_from_name("up") == Direction::Up);
    REQUIRE(!lb::direction_from_name("sideways"));
}

void test_preconditions_are_checked_never_assumed() {
    const auto error_of = [](const std::vector<lb::LabelObservation>& observations, std::int64_t horizon_ns) {
        return chronos::test::failed(label_all(observations, horizon_ns));
    };
    // Time moving backward within an instrument is refused at the row that
    // moved.
    const auto backward = error_of({
        observation("AAPL", 0, 2'000, mid("100")),
        observation("SPY", 1, 1'000, mid("500")),
        observation("AAPL", 2, 1'999, mid("100")),
    }, 1'000);
    REQUIRE(backward.code == ds::DatasetErrorCode::ReceiveTimeMovedBackward);
    REQUIRE(backward.line == 3);
    REQUIRE(error_of({observation("AAPL", 0, -1, mid("1"))}, 1'000).code == ds::DatasetErrorCode::InvalidValue);
    REQUIRE(error_of({}, 0).code == ds::DatasetErrorCode::InvalidArgument);
    REQUIRE(error_of({}, -1).code == ds::DatasetErrorCode::InvalidArgument);
    // Other instruments may interleave earlier times, and nothing at all is
    // fine.
    REQUIRE(labelled({
        observation("AAPL", 0, 2'000, mid("100")),
        observation("SPY", 1, 1'000, mid("500")),
    }, 1'000).size() == 2);
    REQUIRE(labelled({}, 1).empty());
}

// The definition, written as directly as possible: scan forward from each
// row for the first row of its instrument at or after its horizon's end.
std::vector<lb::FutureMidpointReturnRow> reference_labels(
    const std::vector<lb::LabelObservation>& observations,
    std::int64_t horizon_ns
) {
    std::vector<lb::FutureMidpointReturnRow> rows;
    for (std::size_t current = 0; current < observations.size(); ++current) {
        const auto& now = observations[current];
        lb::FutureMidpointReturnRow row{
            .stream = kStream,
            .instrument = now.instrument,
            .ingest_sequence = now.ingest_sequence,
            .prediction_at = now.available_at,
            .horizon_ns = horizon_ns,
            .current_midpoint = now.midpoint,
            .future = std::nullopt,
            .label = LabelUnavailability::NoFutureObservation,
        };
        for (std::size_t later = current + 1; later < observations.size(); ++later) {
            const auto& then = observations[later];
            if (then.instrument == now.instrument &&
                then.available_at.nanoseconds() - now.available_at.nanoseconds() >= horizon_ns) {
                row.future = lb::FutureObservation{
                    .ingest_sequence = then.ingest_sequence,
                    .available_at = then.available_at,
                    .elapsed_ns = then.available_at.nanoseconds() - now.available_at.nanoseconds(),
                    .midpoint = then.midpoint,
                };
                break;
            }
        }
        const auto* current_midpoint = std::get_if<Price>(&now.midpoint);
        const auto* future_midpoint = row.future ? std::get_if<Price>(&row.future->midpoint) : nullptr;
        if (current_midpoint == nullptr) {
            row.label = LabelUnavailability::CurrentMidpointUnavailable;
        } else if (!row.future) {
            row.label = LabelUnavailability::NoFutureObservation;
        } else if (future_midpoint == nullptr) {
            row.label = LabelUnavailability::FutureMidpointUnavailable;
        } else {
            row.label = lb::midpoint_return(*current_midpoint, *future_midpoint);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// Three instruments at different rates, with equal timestamps, bursts and
// gaps, and occasionally unavailable midpoints.
std::vector<lb::LabelObservation> random_observations(std::uint32_t seed, std::size_t count) {
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> pick(0, 9);
    std::uniform_int_distribution<std::int64_t> step(0, 700);
    std::uniform_int_distribution<std::int64_t> cents(9'990, 10'010);
    const std::vector<std::string> instruments = {"AAPL", "SPY", "SPY", "SPY", "QQQ"};
    std::map<std::string, std::int64_t> clock;
    std::vector<lb::LabelObservation> observations;
    std::int64_t now = 0;
    for (std::size_t index = 0; index < count; ++index) {
        now += pick(generator) < 3 ? 0 : step(generator);
        const std::string& instrument = instruments[static_cast<std::size_t>(pick(generator)) % instruments.size()];
        ft::Feature<Price> midpoint = Price::from_raw(cents(generator) * 10'000'000);
        if (pick(generator) == 0) {
            midpoint = ft::Unavailability::MissingBid;
        }
        observations.push_back(observation(instrument, index * 2 + 5, now, midpoint));
    }
    return observations;
}

void test_labels_match_a_brute_force_reference() {
    for (const std::uint32_t seed : {1U, 2U, 3U, 4U, 5U}) {
        const auto observations = random_observations(seed, 600);
        for (const std::int64_t horizon : {1, 500, 1'000, 5'000}) {
            const auto rows = labelled(observations, horizon);
            REQUIRE(rows == reference_labels(observations, horizon));
            std::map<std::uint64_t, std::size_t> position;
            for (std::size_t index = 0; index < observations.size(); ++index) {
                position[observations[index].ingest_sequence] = index;
            }
            for (std::size_t index = 0; index < rows.size(); ++index) {
                const auto& row = rows[index];
                if (!row.future) {
                    continue;
                }
                // Never earlier than the horizon, known exactly when the
                // selected observation is, and the first such row of its
                // instrument.
                const std::size_t selected = position.at(row.future->ingest_sequence);
                REQUIRE(selected > index);
                REQUIRE(row.future->available_at.nanoseconds() >= row.prediction_at.nanoseconds() + horizon);
                REQUIRE(row.future->available_at == observations[selected].available_at);
                REQUIRE(row.future->midpoint == observations[selected].midpoint);
                for (std::size_t between = index + 1; between < selected; ++between) {
                    REQUIRE(observations[between].instrument != row.instrument ||
                        observations[between].available_at.nanoseconds() < row.prediction_at.nanoseconds() + horizon);
                }
            }
        }
    }
}

void test_labels_are_causal() {
    const auto observations = random_observations(11, 500);
    constexpr std::int64_t kHorizon = 1'000;
    const auto full = labelled(observations, kHorizon);
    std::map<std::uint64_t, std::size_t> position;
    for (std::size_t index = 0; index < observations.size(); ++index) {
        position[observations[index].ingest_sequence] = index;
    }
    for (const std::size_t cut : {std::size_t{1}, std::size_t{60}, std::size_t{250}, std::size_t{499}}) {
        // A prefix ends the stream early: a label defined there is exactly
        // the full run's label.
        const std::vector<lb::LabelObservation> prefix(observations.begin(), observations.begin() + static_cast<std::ptrdiff_t>(cut));
        const auto early = labelled(prefix, kHorizon);
        for (std::size_t index = 0; index < early.size(); ++index) {
            if (early[index].future) {
                REQUIRE(early[index] == full[index]);
            } else {
                REQUIRE(!full[index].future || position.at(full[index].future->ingest_sequence) >= cut);
            }
        }
        // Rewriting everything from the cut on, later times and other
        // midpoints, cannot change a label whose observation came before it.
        auto changed = observations;
        for (std::size_t index = cut; index < changed.size(); ++index) {
            changed[index].available_at = Timestamp::from_nanoseconds(changed[index].available_at.nanoseconds() + 777);
            changed[index].midpoint = Price::parse("1");
        }
        const auto altered = labelled(changed, kHorizon);
        for (std::size_t index = 0; index < full.size(); ++index) {
            if (full[index].future && position.at(full[index].future->ingest_sequence) < cut) {
                REQUIRE(altered[index] == full[index]);
            }
        }
    }
}

// AAPL every 1000 ns and SPY every 250 ns; AAPL's third quote is one-sided
// and SPY's last are halted.
std::vector<ValidatedMarketEvent> labelled_session() {
    std::vector<ValidatedMarketEvent> events;
    std::uint64_t sequence = 0;
    const std::vector<std::pair<std::string, std::string>> aapl = {
        {"99.99", "100.01"}, {"100.00", "100.02"}, {"100.01", ""}, {"100.01", "100.03"}, {"99.98", "100.00"},
    };
    for (std::int64_t at = 1'000; at <= 5'000; at += 250) {
        if (at % 1'000 == 0) {
            const auto& [bid, ask] = aapl[static_cast<std::size_t>(at / 1'000 - 1)];
            events.push_back(quote(
                header("AAPL", sequence++, at, at),
                level(bid, "2"),
                ask.empty() ? std::nullopt : level(ask, "1")
            ));
        }
        if (at == 4'500) {
            events.push_back(status(header("SPY", sequence++, at, at), TradingStatus::Halted));
        }
        events.push_back(two_sided("SPY", sequence++, at, at % 500 == 0 ? "500.00" : "500.01", "500.02"));
    }
    return events;
}

void test_an_artifact_holds_one_label_per_feature_row() {
    const FeatureFixture source(labelled_session());
    const TemporaryDirectory root;
    const auto artifact = chronos::test::extracted_labels(source.directory(), root.path(), 1'000);
    REQUIRE(artifact.outcome == ds::PublicationOutcome::Published);
    const auto& manifest = artifact.manifest;
    REQUIRE(manifest.feature_artifact_id == source.artifact().manifest.artifact_id);
    REQUIRE(manifest.features_sha256 == source.artifact().manifest.features.sha256);
    REQUIRE(manifest.dataset_id == source.artifact().manifest.dataset_id);
    REQUIRE(manifest.stream == source.artifact().manifest.stream);
    REQUIRE(manifest.horizon_ns == 1'000);
    REQUIRE(manifest.rows == source.artifact().manifest.rows);
    REQUIRE(manifest.rows_by_instrument == source.artifact().manifest.rows_by_instrument);

    // The rows are exactly the labeler's, from the feature rows in order.
    std::vector<lb::LabelObservation> observations;
    for (const auto& row : chronos::test::feature_rows_of(labelled_session())) {
        observations.push_back(observation(row.instrument.value(), row.ingest_sequence, row.available_at.nanoseconds(), row.midpoint));
    }
    std::string expected;
    std::map<std::string, std::uint64_t> unavailable;
    lb::DirectionCounts directions;
    for (const auto& row : labelled(observations, 1'000)) {
        expected += lb::encode_label_row(row) + "\n";
        if (const auto* value = std::get_if<lb::FutureMidpointReturn>(&row.label)) {
            ++(value->direction == Direction::Down ? directions.down : value->direction == Direction::Flat ? directions.flat : directions.up);
        } else {
            ++unavailable[std::string(lb::label_unavailability_name(std::get<LabelUnavailability>(row.label)))];
        }
    }
    REQUIRE(read_file(artifact.directory / lb::kLabelRowsFileName) == expected);
    REQUIRE(manifest.directions == directions);
    REQUIRE(manifest.unavailable == unavailable);
    // Of 22 rows: AAPL's one-sided quote and three halted SPY quotes have
    // no current midpoint; one AAPL row and three SPY rows observe those
    // first; and one tail row of each instrument is censored.
    REQUIRE(manifest.rows == 22);
    REQUIRE(manifest.unavailable.at("current_midpoint_unavailable") == 4);
    REQUIRE(manifest.unavailable.at("future_midpoint_unavailable") == 4);
    REQUIRE(manifest.unavailable.at("no_future_observation") == 2);
    REQUIRE(lb::usable_labels(manifest) == 12);

    // Decoding gives back the rows, and the row of AAPL's first quote
    // carries its full audit trail.
    const std::string bytes = read_file(artifact.directory / lb::kLabelRowsFileName);
    auto first = chronos::test::succeeded(lb::decode_label_row(std::string_view(bytes).substr(0, bytes.find('\n'))), "decoding");
    REQUIRE(first.instrument.value() == "AAPL");
    REQUIRE(first.prediction_at == Timestamp::from_nanoseconds(1'000));
    REQUIRE(first.future->available_at == Timestamp::from_nanoseconds(2'000));
    REQUIRE(first.future->elapsed_ns == 1'000);
    // (100.01 - 100.00) / 100.00 = 0.0001.
    REQUIRE(value_of(first.label) == (lb::FutureMidpointReturn{.value = lb::Return::parse("0.0001"), .direction = Direction::Up}));

    REQUIRE(chronos::test::succeeded(lb::verify_label_artifact(artifact.directory), "verification") == manifest);
    REQUIRE(!lb::reproduce_label_artifact(manifest, source.directory()));
    // The reader yields exactly the labeler's rows.
    auto reader = chronos::test::succeeded(lb::LabelArtifactReader::open(artifact.directory), "opening");
    std::size_t read = 0;
    while (true) {
        auto next = reader->next();
        REQUIRE(!std::holds_alternative<ds::DatasetError>(next));
        if (std::holds_alternative<ds::EndOfRows>(next)) {
            break;
        }
        ++read;
    }
    REQUIRE(read == manifest.rows);
}

void test_extraction_is_deterministic_and_never_overwrites() {
    const FeatureFixture source(labelled_session());
    const TemporaryDirectory first;
    const TemporaryDirectory second;
    const auto one = chronos::test::extracted_labels(source.directory(), first.path(), 1'000);
    const auto two = chronos::test::extracted_labels(source.directory(), second.path(), 1'000);
    REQUIRE(one.manifest.artifact_id == two.manifest.artifact_id);
    for (const std::string_view file : {lb::kLabelRowsFileName, ds::kArtifactManifestFileName}) {
        REQUIRE(read_file(one.directory / file) == read_file(two.directory / file));
    }
    // Published read-only.
    const auto permissions = std::filesystem::status(one.directory / lb::kLabelRowsFileName).permissions();
    REQUIRE((permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none);
    const std::string before = read_file(one.directory / lb::kLabelRowsFileName);
    const auto again = chronos::test::extracted_labels(source.directory(), first.path(), 1'000);
    REQUIRE(again.outcome == ds::PublicationOutcome::AlreadyPresent);
    REQUIRE(read_file(one.directory / lb::kLabelRowsFileName) == before);
    REQUIRE(chronos::test::entries_in(first.path()) == 1);

    // The horizon and the source are part of the identity.
    const auto other_horizon = chronos::test::extracted_labels(source.directory(), first.path(), 1'001);
    REQUIRE(other_horizon.manifest.artifact_id != one.manifest.artifact_id);
    auto other_events = labelled_session();
    other_events.back() = two_sided("SPY", other_events.size() - 1, 5'000, "400", "600");
    const FeatureFixture other_source(other_events);
    const auto other = chronos::test::extracted_labels(other_source.directory(), first.path(), 1'000);
    REQUIRE(other.manifest.artifact_id != one.manifest.artifact_id);
    REQUIRE(lb::reproduce_label_artifact(one.manifest, other_source.directory())->code == ds::DatasetErrorCode::IdentityMismatch);
    auto wrong_checksum = one.manifest;
    wrong_checksum.features_sha256 = std::string(64, '0');
    wrong_checksum.artifact_id = lb::label_artifact_identity(wrong_checksum);
    REQUIRE(lb::reproduce_label_artifact(wrong_checksum, source.directory())->code == ds::DatasetErrorCode::IdentityMismatch);
    REQUIRE(chronos::test::entries_in(first.path()) == 3);

    // Something else under the artifact's name is a collision, left alone.
    const TemporaryDirectory third;
    std::filesystem::create_directory(third.path() / one.manifest.artifact_id);
    write_file(third.path() / one.manifest.artifact_id / "stray", "x");
    const auto collision = chronos::test::failed(
        lb::extract_future_midpoint_return_labels(source.directory(), third.path(), 1'000)
    );
    REQUIRE(collision.code == ds::DatasetErrorCode::Collision);
    REQUIRE(read_file(third.path() / one.manifest.artifact_id / "stray") == "x");
    REQUIRE(chronos::test::entries_in(third.path()) == 1);

    // A source that fails verification, or a nonpositive horizon, publishes
    // nothing.
    const TemporaryDirectory empty_root;
    REQUIRE(chronos::test::failed(lb::extract_future_midpoint_return_labels(
        source.root() / std::string(64, 'f'), empty_root.path(), 1'000)).code == ds::DatasetErrorCode::NotFound);
    REQUIRE(chronos::test::failed(lb::extract_future_midpoint_return_labels(
        source.directory(), empty_root.path(), 0)).code == ds::DatasetErrorCode::InvalidArgument);
    REQUIRE(chronos::test::entries_in(empty_root.path()) == 0);

    // A feature artifact without rows gives a label artifact without rows.
    const FeatureFixture trades_only({chronos::test::trade(header("AAPL", 0, 1, 1), "100", "1")});
    const auto empty = chronos::test::extracted_labels(trades_only.directory(), empty_root.path(), 1'000);
    REQUIRE(empty.manifest.rows == 0);
    REQUIRE(empty.manifest.labels.size_bytes == 0);
    REQUIRE(lb::usable_labels(empty.manifest) == 0);
    REQUIRE(std::holds_alternative<lb::LabelArtifactManifest>(lb::verify_label_artifact(empty.directory)));
    REQUIRE(!lb::reproduce_label_artifact(empty.manifest, trades_only.directory()));

    // The default horizon is one second.
    const auto default_horizon = chronos::test::succeeded(
        lb::extract_future_midpoint_return_labels(source.directory(), empty_root.path()), "extraction");
    REQUIRE(default_horizon.manifest.horizon_ns == kSecond);
}

void test_label_identity_tracks_its_meaning() {
    const FeatureFixture source(labelled_session());
    const TemporaryDirectory root;
    const auto artifact = chronos::test::extracted_labels(source.directory(), root.path(), 1'000);
    const std::string bytes = read_file(artifact.directory / ds::kArtifactManifestFileName);
    REQUIRE(lb::serialize_label_manifest(artifact.manifest) == bytes);
    REQUIRE(chronos::test::succeeded(lb::parse_label_manifest(bytes), "parsing") == artifact.manifest);
    const auto error_of = [](const std::string& text) { return chronos::test::failed(lb::parse_label_manifest(text)).code; };
    using chronos::test::replaced;
    // Versions and the fixed configuration are part of the meaning.
    REQUIRE(error_of(replaced(bytes, "\"name\":\"chronos.label_artifact\",\"version\":\"1\"",
        "\"name\":\"chronos.label_artifact\",\"version\":\"2\"")) == ds::DatasetErrorCode::UnsupportedVersion);
    REQUIRE(error_of(replaced(bytes, "\"name\":\"future_midpoint_return\",\"version\":\"1\"",
        "\"name\":\"future_midpoint_return\",\"version\":\"2\"")) == ds::DatasetErrorCode::UnsupportedVersion);
    REQUIRE(error_of(replaced(bytes, "\"name\":\"chronos.labels.future_midpoint_return\",\"version\":\"1\"",
        "\"name\":\"chronos.labels.future_midpoint_return\",\"version\":\"2\"")) == ds::DatasetErrorCode::UnsupportedVersion);
    REQUIRE(error_of(replaced(bytes, "\"return_scale\":\"1000000000\"", "\"return_scale\":\"1000000\"")) ==
        ds::DatasetErrorCode::UnsupportedVersion);
    REQUIRE(error_of(replaced(bytes, "\"horizon_ns\":\"1000\"", "\"horizon_ns\":\"0\"")) == ds::DatasetErrorCode::InvalidValue);
    REQUIRE(error_of(replaced(bytes, "\"horizon_ns\":\"1000\"", "\"horizon_ns\":\"01000\"")) == ds::DatasetErrorCode::InvalidValue);
    // Any other edit no longer matches the ID.
    REQUIRE(error_of(replaced(bytes, "\"horizon_ns\":\"1000\"", "\"horizon_ns\":\"2000\"")) == ds::DatasetErrorCode::IdentityMismatch);
    REQUIRE(error_of(replaced(bytes, "\"rows\":\"", "\"rows\":\"1")) == ds::DatasetErrorCode::IdentityMismatch);
    REQUIRE(error_of(replaced(bytes, "{\"artifact_id\"", "{ \"artifact_id\"")) == ds::DatasetErrorCode::NotCanonical);
    REQUIRE(error_of(bytes.substr(0, bytes.size() - 1)) == ds::DatasetErrorCode::MissingFinalNewline);
    // Every source and configuration field changes the ID.
    for (const auto& change : std::vector<void (*)(lb::LabelArtifactManifest&)>{
             [](lb::LabelArtifactManifest& m) { m.feature_artifact_id = std::string(64, '1'); },
             [](lb::LabelArtifactManifest& m) { m.features_sha256 = std::string(64, '2'); },
             [](lb::LabelArtifactManifest& m) { m.dataset_id = std::string(64, '3'); },
             [](lb::LabelArtifactManifest& m) { m.stream = MarketDataStreamId("elsewhere"); },
             [](lb::LabelArtifactManifest& m) { m.horizon_ns += 1; },
         }) {
        auto altered = artifact.manifest;
        change(altered);
        REQUIRE(lb::label_artifact_identity(altered) != artifact.manifest.artifact_id);
    }
    // Self-consistent IDs over inconsistent counts are still refused.
    const auto forged = [](lb::LabelArtifactManifest manifest) {
        manifest.artifact_id = lb::label_artifact_identity(manifest);
        return chronos::test::failed(lb::parse_label_manifest(lb::serialize_label_manifest(manifest))).code;
    };
    auto unknown_reason = artifact.manifest;
    unknown_reason.unavailable["stale"] = 1;
    REQUIRE(forged(unknown_reason) == ds::DatasetErrorCode::InvalidValue);
    auto too_many = artifact.manifest;
    too_many.directions.up += 1;
    REQUIRE(forged(too_many) == ds::DatasetErrorCode::SummaryMismatch);
    auto wrong_instruments = artifact.manifest;
    wrong_instruments.rows_by_instrument["SPY"] += 1;
    REQUIRE(forged(wrong_instruments) == ds::DatasetErrorCode::SummaryMismatch);
    auto empty_with_bytes = artifact.manifest;
    empty_with_bytes.rows = 0;
    empty_with_bytes.rows_by_instrument.clear();
    empty_with_bytes.directions = {};
    empty_with_bytes.unavailable.clear();
    REQUIRE(forged(empty_with_bytes) == ds::DatasetErrorCode::SummaryMismatch);
}

void test_damaged_label_artifacts_fail_verification() {
    const FeatureFixture source(labelled_session());
    const TemporaryDirectory root;
    const auto artifact = chronos::test::extracted_labels(source.directory(), root.path(), 1'000);
    const std::string rows = read_file(artifact.directory / lb::kLabelRowsFileName);
    const TemporaryDirectory copies;
    const auto verdict = [&](std::string_view forged_rows, const lb::LabelArtifactManifest& manifest, bool keep_digest = false) {
        const auto directory = chronos::test::forge_label_artifact(copies.path(), manifest, forged_rows, keep_digest);
        return lb::verify_label_artifact(directory);
    };
    const auto error_of = [&](std::string_view forged_rows, const lb::LabelArtifactManifest& manifest, bool keep_digest = false) {
        return chronos::test::failed(verdict(forged_rows, manifest, keep_digest));
    };
    using chronos::test::replaced;
    const std::size_t second = rows.find('\n') + 1;
    REQUIRE(error_of(rows.substr(second), artifact.manifest).code == ds::DatasetErrorCode::CountMismatch);
    REQUIRE(error_of(rows.substr(0, rows.size() - 1), artifact.manifest).code == ds::DatasetErrorCode::MissingFinalNewline);
    REQUIRE(error_of(rows.substr(second) + rows.substr(0, second), artifact.manifest).code ==
        ds::DatasetErrorCode::IngestSequenceMovedBackward);
    REQUIRE(error_of("{ " + rows.substr(1), artifact.manifest).code == ds::DatasetErrorCode::NotCanonical);
    REQUIRE(error_of("{\"current_midpoint\":" + rows.substr(second), artifact.manifest).code == ds::DatasetErrorCode::MalformedJson);
    REQUIRE(error_of(replaced(rows, "\"label_set\":\"future_midpoint_return\"", "\"label_set\":\"other\""), artifact.manifest).code ==
        ds::DatasetErrorCode::UnsupportedVersion);
    // Same size, different bytes, under the original digest.
    REQUIRE(error_of(replaced(rows, "\"instrument\":\"AAPL\"", "\"instrument\":\"AAPM\""), artifact.manifest, true).code ==
        ds::DatasetErrorCode::ChecksumMismatch);
    auto wrong_counts = artifact.manifest;
    wrong_counts.directions.up -= 1;
    wrong_counts.directions.down += 1;
    REQUIRE(error_of(rows, wrong_counts).code == ds::DatasetErrorCode::SummaryMismatch);
    auto wrong_stream = artifact.manifest;
    wrong_stream.stream = MarketDataStreamId("elsewhere");
    REQUIRE(error_of(rows, wrong_stream).code == ds::DatasetErrorCode::StreamMismatch);
    auto wrong_horizon = artifact.manifest;
    wrong_horizon.horizon_ns = 999;
    REQUIRE(error_of(rows, wrong_horizon).code == ds::DatasetErrorCode::InvalidValue);

    // The first row: AAPL at 1000, observed at 2000 (sequence 2), up.
    const std::string first = rows.substr(0, second - 1);
    REQUIRE(first.find("\"label_available_at_ns\":\"2000\"") != std::string::npos);
    const auto with_first = [&](std::string altered) { return altered + rows.substr(second - 1); };
    // An observation earlier than the horizon.
    const auto early = error_of(with_first(replaced(replaced(first, "\"label_available_at_ns\":\"2000\"",
        "\"label_available_at_ns\":\"1999\""), "\"elapsed_ns\":\"1000\"", "\"elapsed_ns\":\"999\"")), artifact.manifest);
    REQUIRE(early.code == ds::DatasetErrorCode::InvalidValue);
    REQUIRE(early.field == "label_available_at_ns");
    REQUIRE(early.line == 1);
    // An elapsed time that disagrees with the two timestamps.
    REQUIRE(error_of(with_first(replaced(first, "\"elapsed_ns\":\"1000\"", "\"elapsed_ns\":\"1001\"")), artifact.manifest).field ==
        "future.elapsed_ns");
    // A label its midpoints do not give.
    REQUIRE(error_of(with_first(replaced(first, "\"direction\":\"up\"", "\"direction\":\"down\"")), artifact.manifest).field == "label");
    REQUIRE(error_of(with_first(replaced(first, "\"return_raw\":\"100000\"", "\"return_raw\":\"100001\"")), artifact.manifest).field ==
        "label");
    // A future observation without its time, or a time without one.
    REQUIRE(error_of(with_first(replaced(first, "\"label_available_at_ns\":\"2000\"", "\"label_available_at_ns\":null")),
        artifact.manifest).field == "label_available_at_ns");

    // A self-consistent row that skips its first qualifying observation for
    // a later one: only re-deriving the future from the artifact's own rows
    // reveals it. AAPL's row at 2000 qualifies at 3000, where the quote is
    // one-sided; pointing it at 4000 instead keeps every per-row check.
    const std::size_t start = rows.find("\"prediction_at_ns\":\"2000\"");
    const std::size_t row_start = rows.rfind('\n', start) + 1;
    const std::size_t row_end = rows.find('\n', start);
    std::string target = rows.substr(row_start, row_end - row_start);
    REQUIRE(target.find("\"instrument\":\"AAPL\"") != std::string::npos);
    REQUIRE(target.find("future_midpoint_unavailable") != std::string::npos);
    std::string skipped = rows;
    std::string later = replaced(replaced(target, "\"label_available_at_ns\":\"3000\"", "\"label_available_at_ns\":\"4000\""),
        "\"elapsed_ns\":\"1000\"", "\"elapsed_ns\":\"2000\"");
    // The AAPL quote at 4000 is ingest sequence 15 with midpoint 100.02.
    later = replaced(later, "\"midpoint\":{\"unavailable\":\"missing_ask\"}", "\"midpoint\":{\"raw\":\"100020000000\"}");
    later = replaced(later, "\"ingest_sequence\":\"10\",\"midpoint\"", "\"ingest_sequence\":\"15\",\"midpoint\"");
    // (100.02 - 100.01) / 100.01, rounded: 99990.0009999 -> 99990.
    later = replaced(later, "{\"unavailable\":\"future_midpoint_unavailable\"}", "{\"direction\":\"up\",\"return_raw\":\"99990\"}");
    skipped.replace(row_start, target.size(), later);
    auto skipped_counts = artifact.manifest;
    skipped_counts.unavailable["future_midpoint_unavailable"] -= 1;
    skipped_counts.directions.up += 1;
    const auto skip = error_of(skipped, skipped_counts);
    REQUIRE(skip.code == ds::DatasetErrorCode::InvalidValue);
    REQUIRE(skip.field == "future");

    // A plausible but wrong current midpoint on a row no other row observes,
    // with its label adjusted to match, is canonical and self-consistent:
    // only reproduction from the feature artifact reveals it. SPY's first
    // row, at 1000, is nobody's future observation; one billionth more
    // turns its flat label into a down one that still rounds to zero.
    const std::size_t third = rows.find('\n', second) + 1;
    const std::string spy_row = rows.substr(second, third - second - 1);
    REQUIRE(spy_row.find("\"instrument\":\"SPY\"") != std::string::npos);
    std::string wrong_value = rows;
    wrong_value.replace(second, spy_row.size(), replaced(
        replaced(spy_row, "\"current_midpoint\":{\"raw\":\"500010000000\"}", "\"current_midpoint\":{\"raw\":\"500010000001\"}"),
        "{\"direction\":\"flat\",\"return_raw\":\"0\"}",
        "{\"direction\":\"down\",\"return_raw\":\"0\"}"
    ));
    auto moved = artifact.manifest;
    moved.directions.flat -= 1;
    moved.directions.down += 1;
    const auto directory = chronos::test::forge_label_artifact(copies.path(), moved, wrong_value);
    const auto plausible = chronos::test::succeeded(lb::verify_label_artifact(directory), "verification");
    REQUIRE(lb::reproduce_label_artifact(plausible, source.directory())->code == ds::DatasetErrorCode::ChecksumMismatch);
}

void test_forged_feature_values_still_get_typed_labels() {
    // Values no valid dataset produces, and an ordering the dataset
    // contract forbids, in an otherwise valid feature artifact.
    const TemporaryDirectory features;
    const auto directory = chronos::test::forge_feature_artifact(features.path(), {
        chronos::test::midpoint_row("BAD", 0, 0, raw_mid(-1)),
        chronos::test::midpoint_row("HUGE", 1, 0, raw_mid(1)),
        chronos::test::midpoint_row("BAD", 2, 1'000, mid("1")),
        chronos::test::midpoint_row("HUGE", 3, 1'000, raw_mid(kMaximum)),
    });
    REQUIRE(std::holds_alternative<ft::FeatureArtifactManifest>(ft::verify_feature_artifact(directory)));
    const TemporaryDirectory root;
    const auto artifact = chronos::test::extracted_labels(directory, root.path(), 1'000);
    REQUIRE(artifact.manifest.unavailable.at("nonpositive_current_midpoint") == 1);
    REQUIRE(artifact.manifest.unavailable.at("return_overflow") == 1);
    REQUIRE(artifact.manifest.unavailable.at("no_future_observation") == 2);
    REQUIRE(std::holds_alternative<lb::LabelArtifactManifest>(lb::verify_label_artifact(artifact.directory)));
    REQUIRE(!lb::reproduce_label_artifact(artifact.manifest, directory));

    const auto backward = chronos::test::forge_feature_artifact(features.path(), {
        chronos::test::midpoint_row("AAPL", 0, 5'000, mid("100")),
        chronos::test::midpoint_row("AAPL", 1, 4'000, mid("100")),
    });
    const TemporaryDirectory refused;
    const auto error = chronos::test::failed(lb::extract_future_midpoint_return_labels(backward, refused.path(), 1'000));
    REQUIRE(error.code == ds::DatasetErrorCode::ReceiveTimeMovedBackward);
    REQUIRE(error.line == 2);
    REQUIRE(chronos::test::entries_in(refused.path()) == 0);
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"returns are exact and round half away from zero", test_returns_are_exact_and_round_half_away_from_zero},
        {"the first observation at or after the horizon is selected",
         test_the_first_observation_at_or_after_the_horizon_is_selected},
        {"an unavailable first future midpoint is never skipped",
         test_an_unavailable_first_future_midpoint_is_never_skipped},
        {"unavailable labels have one reason each, in order", test_unavailable_labels_have_one_reason_each_in_order},
        {"preconditions are checked, never assumed", test_preconditions_are_checked_never_assumed},
        {"labels match a brute-force reference", test_labels_match_a_brute_force_reference},
        {"labels are causal", test_labels_are_causal},
        {"an artifact holds one label per feature row", test_an_artifact_holds_one_label_per_feature_row},
        {"extraction is deterministic and never overwrites", test_extraction_is_deterministic_and_never_overwrites},
        {"label identity tracks its meaning", test_label_identity_tracks_its_meaning},
        {"damaged label artifacts fail verification", test_damaged_label_artifacts_fail_verification},
        {"forged feature values still get typed labels", test_forged_feature_values_still_get_typed_labels},
    });
}
