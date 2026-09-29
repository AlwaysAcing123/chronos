#pragma once

#include "chronos/market_data_validation.hpp"

#include <optional>
#include <string>
#include <variant>

namespace chronos {

// The reader reached the end of its source cleanly.
struct EndOfMarketEvents {
    // Content evidence for a finite recording, such as its SHA-256, when the
    // reader verified one.
    std::optional<std::string> source_sha256;
};

// The source could not produce its next event. The message is a safe,
// human-readable diagnostic without raw payloads or credentials.
struct MarketEventReadError {
    std::string message;
};

using MarketEventRead = std::variant<
    ValidatedMarketEvent,
    EndOfMarketEvents,
    MarketEventReadError
>;

// A pull source of validated events in normalized order, such as a recorded
// file. Provider adapters implement it, so a replay driver never sees
// provider record types.
class MarketEventReader {
public:
    virtual ~MarketEventReader() = default;

    // Returns the next event, the end of the source, or why neither is
    // possible. After the end or an error it must not be called again.
    [[nodiscard]] virtual MarketEventRead read_next() = 0;
};

}  // namespace chronos
