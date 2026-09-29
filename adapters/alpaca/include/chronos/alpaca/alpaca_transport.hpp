#pragma once

#include <memory>
#include <string>
#include <variant>

namespace chronos::alpaca_adapter {

struct TransportText {
    std::string payload;
};

// Nothing arrived within the transport's poll interval; the connection is
// still open. The live source uses these wakeups to advance market time and
// to notice shutdown.
struct TransportTimeout {};

// A binary frame, which Chronos never requests (Alpaca would send one only
// for MessagePack).
struct TransportBinary {};

struct TransportClosed {
    // Safe to show: never frame content.
    std::string reason;
};

using TransportRead = std::variant<
    TransportText,
    TransportTimeout,
    TransportBinary,
    TransportClosed
>;

// One open WebSocket connection. Only the live source's thread uses it.
class AlpacaConnection {
public:
    virtual ~AlpacaConnection() = default;

    // Returns the next frame, or a timeout within one poll interval.
    [[nodiscard]] virtual TransportRead read() = 0;

    // Sends a text frame; false when the connection is no longer usable.
    [[nodiscard]] virtual bool send(const std::string& text) = 0;
};

struct TransportConnectFailure {
    std::string reason;
};

using TransportConnectResult = std::variant<
    std::unique_ptr<AlpacaConnection>,
    TransportConnectFailure
>;

// Opens connections to one fixed Alpaca stream endpoint. Every blocking
// operation is bounded by a timeout, so the source notices a stop request
// within one poll interval or one connection timeout.
class AlpacaTransport {
public:
    virtual ~AlpacaTransport() = default;

    [[nodiscard]] virtual TransportConnectResult connect() = 0;

    // Asks a blocked connect() or read() to return early. Called from the
    // thread that stops the source. A transport whose operations are
    // already bounded by short timeouts may ignore it.
    virtual void interrupt() noexcept = 0;
};

}  // namespace chronos::alpaca_adapter
