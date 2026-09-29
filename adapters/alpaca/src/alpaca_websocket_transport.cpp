#include "chronos/alpaca/alpaca_websocket_transport.hpp"

#include <httplib.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifndef CPPHTTPLIB_SSL_ENABLED
#error "the Alpaca adapter requires cpp-httplib built with OpenSSL"
#endif

namespace chronos::alpaca_adapter {
namespace {

class WebSocketConnection final : public AlpacaConnection {
public:
    explicit WebSocketConnection(std::unique_ptr<httplib::ws::WebSocketClient> client)
        : client_(std::move(client)) {}

    ~WebSocketConnection() override { client_->close(); }

    WebSocketConnection(const WebSocketConnection&) = delete;
    WebSocketConnection& operator=(const WebSocketConnection&) = delete;
    WebSocketConnection(WebSocketConnection&&) = delete;
    WebSocketConnection& operator=(WebSocketConnection&&) = delete;

    [[nodiscard]] TransportRead read() override {
        std::string message;
        switch (client_->read(message)) {
            case httplib::ws::Text:
                return TransportText{.payload = std::move(message)};
            case httplib::ws::Binary:
                return TransportBinary{};
            case httplib::ws::Timeout:
                return TransportTimeout{};
            case httplib::ws::Fail:
                break;
        }
        return TransportClosed{.reason = "the WebSocket connection closed"};
    }

    [[nodiscard]] bool send(const std::string& text) override {
        return client_->send(text);
    }

private:
    std::unique_ptr<httplib::ws::WebSocketClient> client_;
};

std::string connect_failure(const httplib::ws::Result& result) {
    std::string reason = httplib::to_string(result.error());
    if (result.status() > 0) {
        reason += " (upgrade answered HTTP " + std::to_string(result.status()) + ")";
    }
    return reason;
}

class WebSocketTransport final : public AlpacaTransport {
public:
    WebSocketTransport(std::string url, WebSocketTransportTimeouts timeouts)
        : url_(std::move(url)),
          timeouts_(timeouts) {}

    [[nodiscard]] TransportConnectResult connect() override {
        auto client = std::make_unique<httplib::ws::WebSocketClient>(url_);
        if (!client->is_valid()) {
            return TransportConnectFailure{.reason = "the stream URL is not valid"};
        }
        client->set_connection_timeout(timeouts_.connect);
        // Until the connection is open, reads bound the TLS and WebSocket
        // handshakes; afterwards they bound one poll.
        client->set_read_timeout(timeouts_.connect);
        client->set_write_timeout(timeouts_.write);
        client->set_websocket_ping_interval(
            static_cast<time_t>(timeouts_.ping_interval.count())
        );
        client->set_websocket_max_missed_pongs(timeouts_.maximum_missed_pongs);
        client->enable_server_certificate_verification(true);
        client->enable_server_hostname_verification(true);

        const httplib::ws::Result result = client->connect();
        if (!result) {
            return TransportConnectFailure{.reason = connect_failure(result)};
        }
        client->set_read_timeout(timeouts_.poll);
        return std::unique_ptr<AlpacaConnection>(
            std::make_unique<WebSocketConnection>(std::move(client))
        );
    }

    // Every operation is bounded by a timeout above, so a stop request is
    // noticed without interrupting the socket from another thread.
    void interrupt() noexcept override {}

private:
    std::string url_;
    WebSocketTransportTimeouts timeouts_;
};

bool is_loopback_url(std::string_view url) {
    for (const std::string_view prefix : {
             "ws://127.0.0.1:", "ws://localhost:", "wss://127.0.0.1:", "wss://localhost:",
         }) {
        if (url.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::unique_ptr<AlpacaTransport> make_alpaca_iex_transport(
    WebSocketTransportTimeouts timeouts
) {
    return std::make_unique<WebSocketTransport>(std::string(kAlpacaIexStreamUrl), timeouts);
}

std::unique_ptr<AlpacaTransport> make_loopback_websocket_transport(
    std::string url,
    WebSocketTransportTimeouts timeouts
) {
    if (!is_loopback_url(url)) {
        throw std::invalid_argument("a loopback WebSocket URL must name 127.0.0.1 or localhost");
    }
    return std::make_unique<WebSocketTransport>(std::move(url), timeouts);
}

}  // namespace chronos::alpaca_adapter
