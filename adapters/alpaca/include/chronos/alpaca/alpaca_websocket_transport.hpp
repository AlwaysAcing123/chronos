#pragma once

#include "chronos/alpaca/alpaca_transport.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace chronos::alpaca_adapter {

// The only Alpaca endpoint Chronos connects to: the free IEX stock
// market-data stream. Chronos never calls Alpaca's trading, account, or
// order APIs.
inline constexpr std::string_view kAlpacaIexStreamUrl =
    "wss://stream.data.alpaca.markets/v2/iex";

struct WebSocketTransportTimeouts {
    // The TCP connection, and each read and write of the TLS and WebSocket
    // handshakes.
    std::chrono::milliseconds connect{5'000};
    // How long one read waits for a frame before reporting a timeout. It is
    // also cpp-httplib's limit for the rest of a frame once its first bytes
    // arrive: a longer stall mid-frame closes the connection, and the source
    // reconnects. One second tolerates a typical TCP retransmission while
    // bounding stop latency and live market-time lag to one second.
    std::chrono::milliseconds poll{1'000};
    std::chrono::milliseconds write{5'000};
    // WebSocket pings; a connection that misses this many pongs is closed.
    std::chrono::seconds ping_interval{10};
    int maximum_missed_pongs{2};
};

// WebSocket over TLS to Alpaca's IEX stream, through cpp-httplib and
// OpenSSL. The server certificate and host name are always verified,
// against the system trust store.
[[nodiscard]] std::unique_ptr<AlpacaTransport> make_alpaca_iex_transport(
    WebSocketTransportTimeouts timeouts = {}
);

// The same transport for a WebSocket server on this machine, for tests. It
// refuses any host but 127.0.0.1 and localhost.
[[nodiscard]] std::unique_ptr<AlpacaTransport> make_loopback_websocket_transport(
    std::string url,
    WebSocketTransportTimeouts timeouts = {}
);

}  // namespace chronos::alpaca_adapter
