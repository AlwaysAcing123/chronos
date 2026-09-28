#pragma once

#include "chronos/market_state.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace chronos::test {

// 2026-09-25T13:30:00Z, so rendered timestamps are recognizable.
inline constexpr std::int64_t kMarketOpenNanoseconds =
    1'790'343'000'000'000'000;
inline constexpr std::int64_t kMaximumQuoteAgeNanoseconds = 1'000'000'000;
inline constexpr std::string_view kTestStream = "paper-trading-test-stream";

inline PreTradeRiskLimits paper_risk_limits() {
    return PreTradeRiskLimits{
        .maximum_order_quantity = Quantity::parse("100"),
        .maximum_order_notional = Money::parse("100000"),
        .maximum_position_quantity = Quantity::parse("100"),
        .maximum_quote_age_nanoseconds = kMaximumQuoteAgeNanoseconds,
        .fee_per_fill = Money::parse("0.01"),
    };
}

inline paper_trading::PaperTradingServiceConfig paper_service_config() {
    return paper_trading::PaperTradingServiceConfig{
        .tradeable_instruments = {InstrumentId("AAPL"), InstrumentId("SPY")},
        .account_risk_limits = paper_risk_limits(),
    };
}

struct QuoteLevels {
    std::string_view bid_price{"99"};
    std::string_view bid_quantity{"5"};
    std::string_view ask_price{"101"};
    std::string_view ask_quantity{"5"};
};

inline TopOfBookEvent quote(
    std::uint64_t ingest_sequence,
    std::int64_t offset_nanoseconds,
    std::string instrument = "AAPL",
    QuoteLevels levels = {}
) {
    const std::int64_t received = kMarketOpenNanoseconds + offset_nanoseconds;
    return TopOfBookEvent{
        .header = MarketEventHeader{
            .instrument = InstrumentId(std::move(instrument)),
            .stream = MarketDataStreamId(std::string(kTestStream)),
            .venue = std::nullopt,
            .event_timestamp = Timestamp::from_nanoseconds(received - 250),
            .receive_timestamp = Timestamp::from_nanoseconds(received),
            .ingest_sequence = ingest_sequence,
            .venue_sequence = std::nullopt,
            .quality = MarketDataQuality{}.with(MarketDataQualityFlag::Replay),
        },
        .bid = BookLevel{
            .price = Price::parse(levels.bid_price),
            .quantity = Quantity::parse(levels.bid_quantity),
        },
        .ask = BookLevel{
            .price = Price::parse(levels.ask_price),
            .quantity = Quantity::parse(levels.ask_quantity),
        },
    };
}

inline void apply(MarketState& market_state, MarketEvent event) {
    auto validated = validate_market_event(std::move(event));
    REQUIRE(std::holds_alternative<ValidatedMarketEvent>(validated));
    REQUIRE(std::holds_alternative<MarketReplayStep>(
        market_state.apply(std::get<ValidatedMarketEvent>(validated))
    ));
}

inline std::unique_ptr<MarketState> market_state() {
    return std::make_unique<MarketState>(
        MarketDataStreamId(std::string(kTestStream))
    );
}

inline paper_trading::PaperOrderRequest order_request(
    std::string account_id,
    std::string client_order_id,
    Side side,
    std::string_view quantity,
    OrderType type = OrderType::Market,
    std::optional<std::string_view> limit_price = std::nullopt,
    OrderSourceKind source_kind = OrderSourceKind::Manual
) {
    return paper_trading::PaperOrderRequest{
        .account_id = std::move(account_id),
        .client_order_id = std::move(client_order_id),
        .source = OrderSource{
            .kind = source_kind,
            .source_id = source_kind == OrderSourceKind::Manual
                ? "local-trader"
                : "momentum-v1",
        },
        .instrument = "AAPL",
        .side = side,
        .type = type,
        .quantity = Quantity::parse(quantity),
        .limit_price = limit_price
            ? std::optional<Price>(Price::parse(*limit_price))
            : std::nullopt,
        .time_in_force = TimeInForce::ImmediateOrCancel,
    };
}

}  // namespace chronos::test
