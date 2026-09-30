#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/features/feature_artifact.hpp"
#include "chronos/features/top_of_book_features.hpp"
#include "dataset_test_support.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace ds = chronos::datasets;
namespace ft = chronos::features;
using chronos::MarketDataQuality;
using chronos::MarketDataQualityFlag;
using chronos::Price;
using chronos::Quantity;
using chronos::TradingStatus;
using chronos::ValidatedMarketEvent;
using chronos::test::header;
using chronos::test::level;
using chronos::test::quote;
using chronos::test::read_file;
using chronos::test::status;
using chronos::test::TemporaryDirectory;
using chronos::test::trade;
using chronos::test::write_file;
using ft::Unavailability;

constexpr std::int64_t kMaximum = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMinimum = std::numeric_limits<std::int64_t>::min();

Price price(std::string_view text) { return Price::parse(text); }
Quantity quantity(std::string_view text) { return Quantity::parse(text); }

template <typename Value>
Value value_of(const ft::Feature<Value>& feature) {
    REQUIRE(std::holds_alternative<Value>(feature));
    return std::get<Value>(feature);
}

template <typename Value>
Unavailability reason_of(const ft::Feature<Value>& feature) {
    REQUIRE(std::holds_alternative<Unavailability>(feature));
    return std::get<Unavailability>(feature);
}

std::vector<ft::TopOfBookFeatureRow> rows_of(const std::vector<ValidatedMarketEvent>& events) {
    ft::TopOfBookFeatureEngine engine;
    std::vector<ft::TopOfBookFeatureRow> rows;
    for (const auto& event : events) {
        if (auto row = engine.apply(event)) {
            rows.push_back(std::move(*row));
        }
    }
    return rows;
}

ft::TopOfBookFeatureRow single_row(const ValidatedMarketEvent& event) {
    const auto rows = rows_of({event});
    REQUIRE(rows.size() == 1);
    return rows.front();
}

void test_every_feature_has_its_exact_value() {
    const auto row = single_row(quote(header("AAPL", 7, 1'000, 1'100), level("100", "3"), level("100.02", "1")));
    REQUIRE(value_of(row.best_bid) == price("100"));
    REQUIRE(value_of(row.best_ask) == price("100.02"));
    REQUIRE(value_of(row.bid_size) == quantity("3"));
    REQUIRE(value_of(row.ask_size) == quantity("1"));
    REQUIRE(value_of(row.spread) == price("0.02"));
    REQUIRE(value_of(row.midpoint) == price("100.01"));
    // (3 - 1) / (3 + 1)
    REQUIRE(value_of(row.imbalance) == ft::Ratio::parse("0.5"));
    // (100.02 * 3 + 100 * 1) / 4 = 400.06 / 4
    REQUIRE(value_of(row.microprice) == price("100.015"));
    REQUIRE(row.instrument.value() == "AAPL");
    REQUIRE(row.ingest_sequence == 7);
    REQUIRE(row.event_timestamp.nanoseconds() == 1'000);
    REQUIRE(row.available_at == row.receive_timestamp);
    REQUIRE(!row.trading_status.has_value());
    REQUIRE(ft::encode_feature_row(row) ==
        R"({"available_at_ns":"1100","event_timestamp_ns":"1000","feature_set":"top_of_book",)"
        R"("features":{"ask_size":{"raw":"100000000"},"best_ask":{"raw":"100020000000"},)"
        R"("best_bid":{"raw":"100000000000"},"bid_size":{"raw":"300000000"},)"
        R"("imbalance":{"raw":"500000000"},"microprice":{"raw":"100015000000"},)"
        R"("midpoint":{"raw":"100010000000"},"spread":{"raw":"20000000"}},)"
        R"("ingest_sequence":"7","instrument":"AAPL","quality":["replay"],)"
        R"("receive_timestamp_ns":"1100","stream":"dataset-test-stream",)"
        R"("trading_status":"not_reported","version":"1"})");
}

// Half away from zero, at the price scale for prices and nine decimal
// places for ratios.
void test_ratios_and_means_round_half_away_from_zero() {
    REQUIRE(value_of(ft::midpoint(Price::from_raw(100), Price::from_raw(101))) == Price::from_raw(101));
    REQUIRE(value_of(ft::midpoint(Price::from_raw(-100), Price::from_raw(-101))) == Price::from_raw(-101));
    REQUIRE(value_of(ft::midpoint(Price::from_raw(100), Price::from_raw(102))) == Price::from_raw(101));
    // (B - A) / (B + A) = -0.5e-9 and +0.5e-9 exactly.
    const std::int64_t large = 2'000'000'000;
    REQUIRE(value_of(ft::imbalance(Quantity::from_raw(large - 1), Quantity::from_raw(large + 1))) ==
        ft::Ratio::from_raw(-1));
    REQUIRE(value_of(ft::imbalance(Quantity::from_raw(large + 1), Quantity::from_raw(large - 1))) ==
        ft::Ratio::from_raw(1));
    // -0.4e-9 rounds to zero, which has one spelling.
    const auto small = ft::imbalance(Quantity::from_raw(2'499'999'999), Quantity::from_raw(2'500'000'001));
    REQUIRE(value_of(small) == ft::Ratio::from_raw(0));
    REQUIRE(value_of(small).to_string() == "0");
    // Microprice (3 * 1 + 1 * 2) / 3 = 1.67 raw units rounds to 2;
    // (3 * 1 + 2 * 1) / 2 = 2.5 rounds to 3, and -2.5 to -3.
    REQUIRE(value_of(ft::microprice(Price::from_raw(1), Quantity::from_raw(1), Price::from_raw(3), Quantity::from_raw(2))) ==
        Price::from_raw(2));
    REQUIRE(value_of(ft::microprice(Price::from_raw(2), Quantity::from_raw(1), Price::from_raw(3), Quantity::from_raw(1))) ==
        Price::from_raw(3));
    REQUIRE(value_of(ft::microprice(Price::from_raw(-2), Quantity::from_raw(1), Price::from_raw(-3), Quantity::from_raw(1))) ==
        Price::from_raw(-3));
    REQUIRE(value_of(ft::imbalance(Quantity::from_raw(1), Quantity::from_raw(3))) == ft::Ratio::parse("-0.5"));
    REQUIRE(value_of(ft::imbalance(Quantity::from_raw(1), Quantity::from_raw(2))) ==
        ft::Ratio::from_raw(-333'333'333));
    REQUIRE(value_of(ft::imbalance(Quantity::from_raw(2), Quantity::from_raw(1))) ==
        ft::Ratio::from_raw(333'333'333));
}

// Validated quotes cannot reach these cases; the calculations still refuse
// to produce a value rather than wrap or divide by zero.
void test_zero_denominators_and_overflow_are_unavailable() {
    REQUIRE(reason_of(ft::imbalance(Quantity::from_raw(1), Quantity::from_raw(-1))) == Unavailability::ZeroDenominator);
    REQUIRE(reason_of(ft::microprice(Price::from_raw(1), Quantity::from_raw(1), Price::from_raw(2), Quantity::from_raw(-1))) ==
        Unavailability::ZeroDenominator);
    REQUIRE(reason_of(ft::spread(Price::from_raw(kMinimum), Price::from_raw(kMaximum))) ==
        Unavailability::ArithmeticOverflow);
    REQUIRE(reason_of(ft::microprice(Price::from_raw(kMinimum), Quantity::from_raw(kMinimum),
                                     Price::from_raw(kMinimum), Quantity::from_raw(kMinimum))) ==
        Unavailability::ArithmeticOverflow);
    // Representable intermediate, unrepresentable result.
    REQUIRE(reason_of(ft::microprice(Price::from_raw(0), Quantity::from_raw(kMaximum),
                                     Price::from_raw(kMaximum), Quantity::from_raw(1 - kMaximum))) ==
        Unavailability::ArithmeticOverflow);
    // The largest valid quote is still exact.
    const auto extreme = single_row(quote(
        header("AAPL", 0, 1, 1),
        chronos::BookLevel{.price = Price::from_raw(kMaximum), .quantity = Quantity::from_raw(kMaximum)},
        chronos::BookLevel{.price = Price::from_raw(kMaximum), .quantity = Quantity::from_raw(kMaximum)}
    ));
    REQUIRE(value_of(extreme.microprice) == Price::from_raw(kMaximum));
    REQUIRE(value_of(extreme.midpoint) == Price::from_raw(kMaximum));
    REQUIRE(value_of(extreme.imbalance) == ft::Ratio::from_raw(0));
}

void test_missing_sides_are_typed_never_zero() {
    const auto bid_only = single_row(quote(header("AAPL", 0, 1, 1), level("100", "1"), std::nullopt));
    REQUIRE(value_of(bid_only.best_bid) == price("100"));
    REQUIRE(reason_of(bid_only.best_ask) == Unavailability::MissingAsk);
    REQUIRE(reason_of(bid_only.ask_size) == Unavailability::MissingAsk);
    REQUIRE(reason_of(bid_only.midpoint) == Unavailability::MissingAsk);
    REQUIRE(reason_of(bid_only.microprice) == Unavailability::MissingAsk);
    const auto ask_only = single_row(quote(header("AAPL", 0, 1, 1), std::nullopt, level("100", "1")));
    REQUIRE(reason_of(ask_only.spread) == Unavailability::MissingBid);
    REQUIRE(value_of(ask_only.ask_size) == quantity("1"));
    const auto empty = single_row(quote(header("AAPL", 0, 1, 1), std::nullopt, std::nullopt));
    REQUIRE(reason_of(empty.best_bid) == Unavailability::MissingBid);
    REQUIRE(reason_of(empty.imbalance) == Unavailability::MissingBothSides);
    REQUIRE(ft::encode_feature_row(empty).find("{\"unavailable\":\"missing_both_sides\"}") != std::string::npos);
    // Locked is a real quote: zero spread, and every other value defined.
    const auto locked = single_row(quote(header("AAPL", 0, 1, 1), level("100", "1"), level("100", "3")));
    REQUIRE(value_of(locked.spread) == price("0"));
    REQUIRE(value_of(locked.imbalance) == ft::Ratio::parse("-0.5"));
}

void test_suspect_quotes_and_interruptions_are_unavailable() {
    const auto invalid_book = MarketDataQuality{}.with(MarketDataQualityFlag::PotentiallyInvalidBook);
    const auto crossed = single_row(quote(header("AAPL", 0, 1, 1, invalid_book), level("101", "1"), level("100", "1")));
    REQUIRE(reason_of(crossed.best_bid) == Unavailability::SuspectQuote);
    REQUIRE(reason_of(crossed.spread) == Unavailability::SuspectQuote);
    const auto late = single_row(quote(
        header("AAPL", 0, 5, 1, MarketDataQuality{}.with(MarketDataQualityFlag::QuestionableReceiveTimestamp)),
        level("100", "1"),
        level("101", "1")
    ));
    REQUIRE(reason_of(late.microprice) == Unavailability::SuspectQuote);

    const auto rows = rows_of({
        status(header("AAPL", 0, 1, 1), TradingStatus::Halted),
        quote(header("AAPL", 1, 2, 2), level("100", "1"), level("101", "1")),
        quote(header("SPY", 2, 2, 2), level("500", "1"), level("501", "1")),
        status(header("AAPL", 3, 3, 3), TradingStatus::Open),
        quote(header("AAPL", 4, 4, 4), level("100", "1"), level("101", "1")),
        status(header("SPY", 5, 5, 5), TradingStatus::Closed),
        quote(header("SPY", 6, 6, 6), level("500", "1"), level("501", "1")),
        status(header("AAPL", 7, 7, 7), TradingStatus::PreOpen),
        quote(header("AAPL", 8, 8, 8), level("100", "1"), level("101", "1")),
    });
    REQUIRE(rows.size() == 5);
    REQUIRE(reason_of(rows[0].midpoint) == Unavailability::TradingHalted);
    REQUIRE(reason_of(rows[0].best_bid) == Unavailability::TradingHalted);
    REQUIRE(rows[0].trading_status == TradingStatus::Halted);
    // Each instrument's status is its own.
    REQUIRE(value_of(rows[1].midpoint) == price("500.5"));
    REQUIRE(!rows[1].trading_status.has_value());
    REQUIRE(value_of(rows[2].midpoint) == price("100.5"));
    REQUIRE(reason_of(rows[3].midpoint) == Unavailability::MarketClosed);
    REQUIRE(value_of(rows[4].midpoint) == price("100.5"));
    REQUIRE(rows[4].trading_status == TradingStatus::PreOpen);
}

void test_rows_start_only_once_a_quote_exists() {
    const auto definition = chronos::test::valid(chronos::InstrumentDefinitionEvent{
        .header = header("AAPL", 2, 3, 3),
        .display_symbol = "AAPL",
        .currency = chronos::CurrencyCode("USD"),
        .minimum_price_increment = price("0.01"),
        .minimum_quantity_increment = quantity("1"),
        .effective_from = chronos::Timestamp::from_nanoseconds(0),
        .effective_until = std::nullopt,
    });
    ft::TopOfBookFeatureEngine engine;
    REQUIRE(!engine.apply(status(header("AAPL", 0, 1, 1), TradingStatus::Open)));
    REQUIRE(!engine.apply(trade(header("AAPL", 1, 2, 2), "100", "1")));
    REQUIRE(!engine.apply(definition));
    const auto row = engine.apply(quote(header("AAPL", 3, 4, 4), level("100", "1"), level("101", "1")));
    REQUIRE(row.has_value());
    REQUIRE(row->ingest_sequence == 3);
    REQUIRE(row->trading_status == TradingStatus::Open);
    REQUIRE(!engine.apply(trade(header("AAPL", 4, 5, 5), "100.5", "1")));
}

// Equal timestamps: order is the ingest order the events arrive in.
std::vector<ValidatedMarketEvent> busy_session() {
    std::vector<ValidatedMarketEvent> events;
    std::uint64_t sequence = 0;
    for (int step = 0; step < 12; ++step) {
        const std::int64_t time = 1'000 + (step / 3) * 10;
        const std::string instrument = step % 2 == 0 ? "AAPL" : "SPY";
        events.push_back(quote(
            header(instrument, sequence++, time, time),
            level(step % 2 == 0 ? "100" : "500", std::to_string(1 + step)),
            level(step % 2 == 0 ? "100.01" : "500.02", std::to_string(12 - step))
        ));
        if (step % 4 == 3) {
            events.push_back(trade(header(instrument, sequence++, time, time), "100", "1"));
        }
    }
    return events;
}

void test_rows_follow_ingest_order_and_never_see_the_future() {
    const auto events = busy_session();
    const auto rows = rows_of(events);
    REQUIRE(rows.size() == 12);
    for (std::size_t index = 1; index < rows.size(); ++index) {
        REQUIRE(rows[index - 1].ingest_sequence < rows[index].ingest_sequence);
    }
    // Every prefix of the stream yields a prefix of the rows: no row can
    // change once a later event arrives.
    for (std::size_t length = 0; length <= events.size(); ++length) {
        const auto prefix = rows_of(std::vector<ValidatedMarketEvent>(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(length)));
        REQUIRE(prefix.size() <= rows.size());
        for (std::size_t index = 0; index < prefix.size(); ++index) {
            REQUIRE(ft::encode_feature_row(prefix[index]) == ft::encode_feature_row(rows[index]));
        }
    }
    // Changing the last quote changes only its own row.
    auto changed = events;
    changed[events.size() - 2] = quote(header("SPY", events.size() - 2, 1'030, 1'030), level("400", "9"), level("600", "9"));
    const auto changed_rows = rows_of(changed);
    REQUIRE(changed_rows.size() == rows.size());
    for (std::size_t index = 0; index + 1 < rows.size(); ++index) {
        REQUIRE(ft::encode_feature_row(changed_rows[index]) == ft::encode_feature_row(rows[index]));
    }
    REQUIRE(ft::encode_feature_row(changed_rows.back()) != ft::encode_feature_row(rows.back()));
}

std::string expected_rows_file(const std::vector<ValidatedMarketEvent>& events) {
    std::string bytes;
    for (const auto& row : rows_of(events)) {
        bytes += ft::encode_feature_row(row) + "\n";
    }
    return bytes;
}

ft::PublishedFeatureArtifact extracted(const std::filesystem::path& dataset, const std::filesystem::path& root) {
    auto result = ft::extract_top_of_book_features(dataset, root);
    if (auto* error = std::get_if<ds::DatasetError>(&result)) {
        throw chronos::test::TestFailure("extraction failed: " + error->field + ": " + error->message);
    }
    return std::get<ft::PublishedFeatureArtifact>(std::move(result));
}

void test_an_artifact_holds_exactly_the_engines_rows() {
    const TemporaryDirectory catalog;
    const TemporaryDirectory features;
    const auto events = busy_session();
    const auto dataset = chronos::test::published(chronos::test::write_dataset(catalog.path(), events));
    const auto artifact = extracted(dataset.directory, features.path());
    REQUIRE(artifact.outcome == ds::PublicationOutcome::Published);
    // Rows from events in memory and rows from the normalized dataset are
    // the same bytes.
    REQUIRE(read_file(artifact.directory / ft::kFeatureRowsFileName) == expected_rows_file(events));
    REQUIRE(artifact.manifest.rows == 12);
    REQUIRE(artifact.manifest.rows_by_instrument.at("AAPL") == 6);
    REQUIRE(artifact.manifest.dataset_id == dataset.manifest.dataset_id);
    REQUIRE(artifact.manifest.dataset_events_sha256 == dataset.manifest.events.sha256);
    REQUIRE(artifact.manifest.unavailable.empty());
    REQUIRE(std::holds_alternative<ft::FeatureArtifactManifest>(ft::verify_feature_artifact(artifact.directory)));
    REQUIRE(!ft::reproduce_feature_artifact(artifact.manifest, dataset.directory));
    // Every decoded row is exactly the engine's.
    const auto rows = rows_of(events);
    std::size_t index = 0;
    std::string bytes = read_file(artifact.directory / ft::kFeatureRowsFileName);
    for (std::size_t start = 0; start < bytes.size(); ++index) {
        const std::size_t end = bytes.find('\n', start);
        auto decoded = ft::decode_feature_row(std::string_view(bytes).substr(start, end - start));
        REQUIRE(std::holds_alternative<ft::TopOfBookFeatureRow>(decoded));
        REQUIRE(std::get<ft::TopOfBookFeatureRow>(decoded) == rows[index]);
        start = end + 1;
    }
    REQUIRE(index == rows.size());
}

void test_extraction_is_reproducible_and_never_overwrites() {
    const TemporaryDirectory catalog;
    const TemporaryDirectory first;
    const TemporaryDirectory second;
    const auto events = busy_session();
    const auto dataset = chronos::test::published(chronos::test::write_dataset(catalog.path(), events));
    const auto one = extracted(dataset.directory, first.path());
    const auto two = extracted(dataset.directory, second.path());
    REQUIRE(one.manifest.artifact_id == two.manifest.artifact_id);
    for (const std::string_view file : {ft::kFeatureRowsFileName, ft::kFeatureManifestFileName}) {
        REQUIRE(read_file(one.directory / file) == read_file(two.directory / file));
    }
    const auto again = extracted(dataset.directory, first.path());
    REQUIRE(again.outcome == ds::PublicationOutcome::AlreadyPresent);
    REQUIRE(std::distance(std::filesystem::directory_iterator(first.path()), std::filesystem::directory_iterator()) == 1);

    // A later event altered in the source: every earlier row is unchanged,
    // but the artifact is a different one.
    auto changed = events;
    changed[events.size() - 2] = quote(header("SPY", events.size() - 2, 1'030, 1'030), level("400", "9"), level("600", "9"));
    const TemporaryDirectory other_catalog;
    const auto other_dataset = chronos::test::published(chronos::test::write_dataset(other_catalog.path(), changed));
    const auto other = extracted(other_dataset.directory, first.path());
    REQUIRE(other.manifest.artifact_id != one.manifest.artifact_id);
    const std::string original = read_file(one.directory / ft::kFeatureRowsFileName);
    const std::string altered = read_file(other.directory / ft::kFeatureRowsFileName);
    const std::size_t last = original.rfind('\n', original.size() - 2) + 1;
    REQUIRE(altered.substr(0, last) == original.substr(0, last));
    REQUIRE(altered.substr(last) != original.substr(last));
    // The other dataset does not reproduce this artifact.
    REQUIRE(ft::reproduce_feature_artifact(one.manifest, other_dataset.directory)->code ==
        ds::DatasetErrorCode::IdentityMismatch);

    // A dataset with no quotes yields an empty artifact, whose stream only
    // reproduction can check.
    const TemporaryDirectory trades_catalog;
    const auto trades_only = chronos::test::published(chronos::test::write_dataset(
        trades_catalog.path(), {trade(header("AAPL", 0, 1, 1), "100", "1")}
    ));
    const TemporaryDirectory empty_features;
    const auto empty = extracted(trades_only.directory, empty_features.path());
    REQUIRE(empty.manifest.rows == 0);
    REQUIRE(empty.manifest.features.size_bytes == 0);
    REQUIRE(std::holds_alternative<ft::FeatureArtifactManifest>(ft::verify_feature_artifact(empty.directory)));
    REQUIRE(!ft::reproduce_feature_artifact(empty.manifest, trades_only.directory));
    auto restreamed = empty.manifest;
    restreamed.stream = chronos::MarketDataStreamId("elsewhere");
    restreamed.artifact_id = ft::feature_artifact_identity(restreamed);
    REQUIRE(ft::reproduce_feature_artifact(restreamed, trades_only.directory)->code ==
        ds::DatasetErrorCode::IdentityMismatch);

    // A dataset that fails verification yields no artifact.
    const TemporaryDirectory empty_root;
    auto result = ft::extract_top_of_book_features(catalog.path() / std::string(64, 'f'), empty_root.path());
    REQUIRE(std::holds_alternative<ds::DatasetError>(result));
    REQUIRE(std::filesystem::directory_iterator(empty_root.path()) == std::filesystem::directory_iterator());
}

void test_artifact_identity_tracks_its_meaning() {
    const TemporaryDirectory catalog;
    const TemporaryDirectory features;
    const auto dataset = chronos::test::published(chronos::test::write_dataset(catalog.path(), busy_session()));
    const auto artifact = extracted(dataset.directory, features.path());
    const std::string bytes = read_file(artifact.directory / ft::kFeatureManifestFileName);
    auto other_source = artifact.manifest;
    other_source.dataset_id = std::string(64, 'd');
    REQUIRE(ft::feature_artifact_identity(other_source) != artifact.manifest.artifact_id);
    const auto error_of = [](const std::string& text) {
        auto parsed = ft::parse_feature_manifest(text);
        REQUIRE(std::holds_alternative<ds::DatasetError>(parsed));
        return std::get<ds::DatasetError>(parsed).code;
    };
    const auto replaced = [](std::string text, std::string_view from, std::string_view to) {
        const std::size_t position = text.find(from);
        REQUIRE(position != std::string::npos);
        return text.replace(position, from.size(), to);
    };
    // The feature version and its configuration are part of the meaning; a
    // reader of version 1 refuses anything else.
    REQUIRE(error_of(replaced(bytes, "\"name\":\"top_of_book\",\"version\":\"1\"", "\"name\":\"top_of_book\",\"version\":\"2\"")) ==
        ds::DatasetErrorCode::UnsupportedVersion);
    REQUIRE(error_of(replaced(bytes, "\"ratio_scale\":\"1000000000\"", "\"ratio_scale\":\"1000000\"")) ==
        ds::DatasetErrorCode::UnsupportedVersion);
    REQUIRE(error_of(replaced(bytes, "\"rows\":\"12\"", "\"rows\":\"13\"")) == ds::DatasetErrorCode::IdentityMismatch);
    auto unknown_reason = artifact.manifest;
    unknown_reason.unavailable["spread"]["future_reason"] = 1;
    unknown_reason.artifact_id = ft::feature_artifact_identity(unknown_reason);
    REQUIRE(error_of(ft::serialize_feature_manifest(unknown_reason)) == ds::DatasetErrorCode::InvalidValue);
    auto reparsed = ft::parse_feature_manifest(bytes);
    REQUIRE(std::get<ft::FeatureArtifactManifest>(reparsed) == artifact.manifest);
}

void test_damaged_artifacts_fail_verification() {
    const TemporaryDirectory catalog;
    const TemporaryDirectory features;
    const auto dataset = chronos::test::published(chronos::test::write_dataset(catalog.path(), busy_session()));
    const auto artifact = extracted(dataset.directory, features.path());
    const std::string rows = read_file(artifact.directory / ft::kFeatureRowsFileName);
    const TemporaryDirectory copies;
    const auto forge = [&](std::string_view forged_rows, ft::FeatureArtifactManifest manifest) {
        manifest.features = ds::FileDigest{.sha256 = chronos::Sha256::hex(forged_rows), .size_bytes = forged_rows.size()};
        manifest.artifact_id = ft::feature_artifact_identity(manifest);
        const auto directory = copies.path() / manifest.artifact_id;
        std::filesystem::create_directory(directory);
        write_file(directory / ft::kFeatureRowsFileName, forged_rows);
        write_file(directory / ft::kFeatureManifestFileName, ft::serialize_feature_manifest(manifest));
        return std::pair(directory, manifest);
    };
    const auto forged = [&](std::string_view forged_rows, const ft::FeatureArtifactManifest& manifest) {
        auto verified = ft::verify_feature_artifact(forge(forged_rows, manifest).first);
        REQUIRE(std::holds_alternative<ds::DatasetError>(verified));
        return std::get<ds::DatasetError>(verified).code;
    };
    const std::size_t second = rows.find('\n') + 1;
    REQUIRE(forged(rows.substr(second), artifact.manifest) == ds::DatasetErrorCode::CountMismatch);
    REQUIRE(forged(rows.substr(0, rows.size() - 1), artifact.manifest) == ds::DatasetErrorCode::MissingFinalNewline);
    REQUIRE(forged(rows.substr(second) + rows.substr(0, second), artifact.manifest) ==
        ds::DatasetErrorCode::IngestSequenceMovedBackward);
    REQUIRE(forged("{ " + rows.substr(1), artifact.manifest) == ds::DatasetErrorCode::NotCanonical);
    std::string unknown_reason = rows;
    unknown_reason.replace(unknown_reason.find("{\"raw\":\"100000000\"}"), 19, "{\"unavailable\":\"stale\"}");
    REQUIRE(forged(unknown_reason, artifact.manifest) == ds::DatasetErrorCode::InvalidValue);
    auto wrong_counts = artifact.manifest;
    wrong_counts.unavailable["spread"]["missing_bid"] = 1;
    REQUIRE(forged(rows, wrong_counts) == ds::DatasetErrorCode::SummaryMismatch);
    auto wrong_stream = artifact.manifest;
    wrong_stream.stream = chronos::MarketDataStreamId("elsewhere");
    REQUIRE(forged(rows, wrong_stream) == ds::DatasetErrorCode::StreamMismatch);

    // A plausible but wrong value is canonical and self-consistent, so only
    // recomputing from the source dataset can reveal it.
    std::string wrong_value = rows;
    const std::string_view right_bid = "\"best_bid\":{\"raw\":\"100000000000\"}";
    wrong_value.replace(wrong_value.find(right_bid), right_bid.size(), "\"best_bid\":{\"raw\":\"100000000001\"}");
    const auto [directory, manifest] = forge(wrong_value, artifact.manifest);
    REQUIRE(std::holds_alternative<ft::FeatureArtifactManifest>(ft::verify_feature_artifact(directory)));
    REQUIRE(ft::reproduce_feature_artifact(manifest, dataset.directory)->code == ds::DatasetErrorCode::ChecksumMismatch);
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"every feature has its exact value", test_every_feature_has_its_exact_value},
        {"ratios and means round half away from zero", test_ratios_and_means_round_half_away_from_zero},
        {"zero denominators and overflow are unavailable", test_zero_denominators_and_overflow_are_unavailable},
        {"missing sides are typed, never zero", test_missing_sides_are_typed_never_zero},
        {"suspect quotes and interruptions are unavailable",
         test_suspect_quotes_and_interruptions_are_unavailable},
        {"rows start only once a quote exists", test_rows_start_only_once_a_quote_exists},
        {"rows follow ingest order and never see the future",
         test_rows_follow_ingest_order_and_never_see_the_future},
        {"an artifact holds exactly the engine's rows", test_an_artifact_holds_exactly_the_engines_rows},
        {"extraction is reproducible and never overwrites", test_extraction_is_reproducible_and_never_overwrites},
        {"artifact identity tracks its meaning", test_artifact_identity_tracks_its_meaning},
        {"damaged artifacts fail verification", test_damaged_artifacts_fail_verification},
    });
}
