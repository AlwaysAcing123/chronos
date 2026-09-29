#include "chronos/alpaca/alpaca_subscription.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::alpaca_adapter {
namespace {

constexpr std::size_t kMaximumSymbolLength = 16;

bool is_supported_symbol(std::string_view symbol) {
    if (symbol.empty() || symbol.size() > kMaximumSymbolLength) {
        return false;
    }
    return std::all_of(symbol.begin(), symbol.end(), [](char character) {
        return (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '.';
    });
}

std::optional<std::string> channel_mismatch(
    std::string_view channel,
    const std::vector<InstrumentId>& expected,
    const std::vector<std::string>& acknowledged
) {
    std::set<std::string, std::less<>> wanted;
    for (const InstrumentId& instrument : expected) {
        wanted.insert(instrument.value());
    }
    const std::set<std::string, std::less<>> received(
        acknowledged.begin(),
        acknowledged.end()
    );
    for (const std::string& symbol : wanted) {
        if (!received.contains(symbol)) {
            return "Alpaca did not subscribe " + std::string(channel) + " for " +
                symbol;
        }
    }
    for (const std::string& symbol : received) {
        if (!wanted.contains(symbol)) {
            return "Alpaca subscribed " + std::string(channel) +
                " for the unrequested symbol " + printable_excerpt(symbol);
        }
    }
    return std::nullopt;
}

}  // namespace

std::variant<AlpacaSubscription, AlpacaSubscriptionError> AlpacaSubscription::create(
    const std::vector<InstrumentId>& instruments
) {
    std::vector<InstrumentId> unique;
    std::set<std::string, std::less<>> seen;
    for (const InstrumentId& instrument : instruments) {
        if (!is_supported_symbol(instrument.value())) {
            return AlpacaSubscriptionError{
                .message = "Alpaca symbols use only A-Z, 0-9, and '.', at most 16 "
                           "characters: " + printable_excerpt(instrument.value()),
            };
        }
        if (seen.insert(instrument.value()).second) {
            unique.push_back(instrument);
        }
    }
    if (unique.empty()) {
        return AlpacaSubscriptionError{
            .message = "an Alpaca subscription needs at least one symbol",
        };
    }
    if (unique.size() > kAlpacaBasicSymbolLimit) {
        return AlpacaSubscriptionError{
            .message = "Alpaca IEX on the Basic plan supports at most " +
                std::to_string(kAlpacaBasicSymbolLimit) + " symbols; " +
                std::to_string(unique.size()) + " were configured",
        };
    }
    return AlpacaSubscription(std::move(unique));
}

const InstrumentId* AlpacaSubscription::find(std::string_view symbol) const noexcept {
    for (const InstrumentId& instrument : instruments_) {
        if (instrument.value() == symbol) {
            return &instrument;
        }
    }
    return nullptr;
}

std::string AlpacaSubscription::subscribe_message() const {
    nlohmann::ordered_json symbols = nlohmann::ordered_json::array();
    for (const InstrumentId& instrument : instruments_) {
        symbols.push_back(instrument.value());
    }
    nlohmann::ordered_json message = nlohmann::ordered_json::object();
    message["action"] = "subscribe";
    message["trades"] = symbols;
    message["quotes"] = symbols;
    return message.dump();
}

std::optional<std::string> AlpacaSubscription::acknowledgement_mismatch(
    const AlpacaSubscriptionMessage& acknowledgement
) const {
    if (auto mismatch = channel_mismatch("trades", instruments_, acknowledgement.trades)) {
        return mismatch;
    }
    return channel_mismatch("quotes", instruments_, acknowledgement.quotes);
}

}  // namespace chronos::alpaca_adapter
