#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::alpaca_adapter {

// Alpaca's stock stream sends every WebSocket text frame as a JSON array of
// flat objects, each tagged by "T". This decoder turns one frame into typed
// messages without interpreting any market value: prices and sizes stay the
// exact text of their JSON numbers, so nothing passes through binary
// floating point. The normalizer gives them meaning.
//
// Fields Chronos does not read are ignored, so additive protocol changes do
// not break valid messages; a required field that is missing or has the
// wrong JSON type is an error, as is an unknown message type.

struct AlpacaSuccess {
    std::string message;
};

struct AlpacaErrorMessage {
    std::int64_t code{0};
    std::string message;
};

struct AlpacaSubscriptionMessage {
    std::vector<std::string> trades;
    std::vector<std::string> quotes;
};

struct AlpacaQuote {
    std::string symbol;
    std::optional<std::string> bid_exchange;
    std::string bid_price;
    std::string bid_size;
    std::optional<std::string> ask_exchange;
    std::string ask_price;
    std::string ask_size;
    std::string timestamp;
    std::vector<std::string> conditions;
    std::optional<std::string> tape;
};

struct AlpacaTrade {
    std::string symbol;
    std::string trade_id;
    std::string exchange;
    std::string price;
    std::string size;
    std::string timestamp;
    std::vector<std::string> conditions;
    std::optional<std::string> tape;
};

// A documented message type Chronos deliberately does not apply: trade
// corrections ("c") and cancels ("x"), which a trade subscription receives
// automatically, and channels Chronos never subscribes to.
struct AlpacaIgnoredMessage {
    std::string type;
};

using AlpacaMessage = std::variant<
    AlpacaSuccess,
    AlpacaErrorMessage,
    AlpacaSubscriptionMessage,
    AlpacaQuote,
    AlpacaTrade,
    AlpacaIgnoredMessage
>;

struct AlpacaDecodeError {
    // A safe explanation naming the message and field, never the frame.
    std::string message;
};

using AlpacaDecodeResult = std::variant<
    std::vector<AlpacaMessage>,
    AlpacaDecodeError
>;

[[nodiscard]] AlpacaDecodeResult decode_alpaca_frame(std::string_view frame);

[[nodiscard]] bool is_control_message(const AlpacaMessage& message) noexcept;

// Text from a message, made safe for a diagnostic: printable ASCII only and
// bounded in length.
[[nodiscard]] std::string printable_excerpt(std::string_view text);

}  // namespace chronos::alpaca_adapter
