#pragma once

#include "chronos/alpaca/alpaca_protocol.hpp"
#include "chronos/trading_types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::alpaca_adapter {

// Alpaca's free Basic plan allows 30 symbols on its WebSocket stream.
// Chronos enforces the limit itself rather than learning it from a
// rejected subscription.
inline constexpr std::size_t kAlpacaBasicSymbolLimit = 30;

struct AlpacaSubscriptionError {
    std::string message;
};

// The symbols one connection subscribes to, for both trades and quotes.
class AlpacaSubscription final {
public:
    // Removes repeated symbols, keeping each at its first position, and
    // rejects an empty list, a symbol Alpaca would not recognize (only A-Z,
    // 0-9, and '.', at most 16 characters), and more than 30 symbols.
    [[nodiscard]] static std::variant<AlpacaSubscription, AlpacaSubscriptionError>
    create(const std::vector<InstrumentId>& instruments);

    [[nodiscard]] const std::vector<InstrumentId>& instruments() const noexcept {
        return instruments_;
    }

    [[nodiscard]] const InstrumentId* find(std::string_view symbol) const noexcept;

    // {"action":"subscribe","trades":[...],"quotes":[...]}
    [[nodiscard]] std::string subscribe_message() const;

    // Why an acknowledgement does not describe exactly this subscription,
    // or nullopt when it does. Other channels are not compared: Alpaca adds
    // corrections and cancels to every trade subscription.
    [[nodiscard]] std::optional<std::string> acknowledgement_mismatch(
        const AlpacaSubscriptionMessage& acknowledgement
    ) const;

private:
    explicit AlpacaSubscription(std::vector<InstrumentId> instruments)
        : instruments_(std::move(instruments)) {}

    std::vector<InstrumentId> instruments_;
};

}  // namespace chronos::alpaca_adapter
