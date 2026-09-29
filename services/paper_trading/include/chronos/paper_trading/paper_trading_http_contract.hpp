#pragma once

#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_data_source.hpp"
#include "chronos/paper_trading/http_message.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"

#include <cstdint>

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace chronos::paper_trading {

// The JSON contract of the paper-trading HTTP API; see
// docs/paper-trading-api.md.
//
// Requests are strict. A body or query string that cannot be converted into
// a typed request is rejected here, before the service is called, and is
// never journaled.
// Well-formed values that violate domain rules, such as a zero quantity,
// reach the risk engine and are journaled as rejections.
//
// Responses never contain JSON numbers: decimals are canonical decimal
// strings, integers are base-10 strings, and timestamps are RFC 3339 UTC
// strings with nine fractional digits.

enum class MalformedRequestReason {
    InvalidJson,
    InvalidType,
    MissingField,
    UnexpectedField,
    DuplicateField,
    InvalidDecimal,
    TooManyFractionalDigits,
    DecimalOutOfRange,
    UnknownEnumValue,
    InvalidQuery,
    InvalidLimit,
};

struct MalformedRequest {
    MalformedRequestReason reason;
    std::string field;
    std::string message;
};

[[nodiscard]] std::variant<CreatePaperAccountRequest, MalformedRequest>
parse_create_account_request(std::string_view body);

[[nodiscard]] std::variant<PaperOrderRequest, MalformedRequest>
parse_order_request(std::string account_id, std::string_view body);

// Accepts only `limit`, `cursor`, and `instrument`, each at most once. Range,
// cursor, and identifier validity are the service's rules, so they apply to
// every transport.
[[nodiscard]] std::variant<HistoryPageRequest, MalformedRequest>
parse_history_query(std::string_view query);

// For endpoints without parameters: any query parameter is rejected rather
// than silently ignored.
[[nodiscard]] std::optional<MalformedRequest> reject_query_parameters(
    std::string_view query
);

// For commands without parameters: the body must be exactly an empty JSON
// object, so a mistyped or misplaced option is rejected, not ignored.
[[nodiscard]] std::optional<MalformedRequest> parse_empty_object_request(
    std::string_view body
);

// The market-data resource: the source, the stream, and whether each
// tradeable instrument has an execution quote now.
struct MarketDataView {
    market_data::MarketDataSourceStatus source;
    MarketAvailability market;
    // The notifier revision this view is at least as new as.
    std::uint64_t revision;
};

[[nodiscard]] HttpResponse render_market_data(const MarketDataView& view);

[[nodiscard]] HttpResponse render_replay_command_error(
    const market_data::ReplayCommandError& error,
    market_data::MarketDataMode mode
);

[[nodiscard]] HttpResponse render_event_stream_unavailable(
    market_data::MarketDataNotifier::SubscribeError error
);

// Server-Sent Events text. The preamble sets the browser's reconnect delay;
// each notice is one event or comment, and nullopt ends the stream.
inline constexpr std::string_view kEventStreamPreamble = "retry: 3000\n\n";
[[nodiscard]] std::optional<std::string> event_stream_text(
    const market_data::MarketDataNotice& notice
);

[[nodiscard]] std::string format_timestamp(Timestamp timestamp);

// With a source status, the body also reports the market-data source's
// mode, label, and lifecycle state, such as a live feed still connecting.
[[nodiscard]] HttpResponse render_health(
    const PaperTradingHealth& health,
    const std::optional<market_data::MarketDataSourceStatus>& source = std::nullopt
);

[[nodiscard]] HttpResponse render_account(
    const PaperAccountResult& result,
    int success_status
);

[[nodiscard]] HttpResponse render_order_submission(
    const PaperOrderSubmissionResult& result
);

[[nodiscard]] HttpResponse render_order_history(
    const PaperOrderHistoryResult& result
);

[[nodiscard]] HttpResponse render_order_record(
    const PaperOrderRecordResult& result
);

[[nodiscard]] HttpResponse render_fill_history(
    const PaperFillHistoryResult& result
);

[[nodiscard]] HttpResponse render_portfolio(const PaperPortfolioResult& result);

[[nodiscard]] HttpResponse render_malformed_request(
    const MalformedRequest& malformed
);

[[nodiscard]] HttpResponse render_error(
    int status,
    std::string_view code,
    std::string_view message
);

}  // namespace chronos::paper_trading
