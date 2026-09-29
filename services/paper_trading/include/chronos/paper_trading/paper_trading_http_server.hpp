#pragma once

#include "chronos/paper_trading/paper_trading_http_api.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace chronos::paper_trading {

struct HttpServerOptions {
    // Development servers bind to loopback. The API has no authentication
    // and must not be exposed to other hosts.
    std::string host{"127.0.0.1"};
    // Zero selects an ephemeral port, which bind() reports.
    int port{8080};
    std::size_t maximum_request_body_bytes{16 * 1024};
    std::chrono::seconds read_timeout{5};
    std::chrono::seconds write_timeout{5};
    // Threads for ordinary requests. Each open event stream holds one more,
    // reserved up to the API's event-stream capacity, so streams can never
    // starve ordinary requests.
    std::size_t worker_threads{4};
};

// Serves PaperTradingHttpApi over HTTP/1.1. This is the only component that
// depends on the HTTP library. Responses are JSON or, for the event stream,
// Server-Sent Events; none is cached, and all carry
// `X-Chronos-Trading-Mode: paper`. `api` must outlive the server.
class PaperTradingHttpServer final {
public:
    PaperTradingHttpServer(
        PaperTradingHttpApi& api,
        HttpServerOptions options,
        DiagnosticLog access_log
    );
    ~PaperTradingHttpServer();

    PaperTradingHttpServer(const PaperTradingHttpServer&) = delete;
    PaperTradingHttpServer& operator=(const PaperTradingHttpServer&) = delete;
    PaperTradingHttpServer(PaperTradingHttpServer&&) = delete;
    PaperTradingHttpServer& operator=(PaperTradingHttpServer&&) = delete;

    // Binds the listening socket and returns its port. Throws
    // std::runtime_error when the address cannot be bound.
    [[nodiscard]] int bind();

    // Serves requests on the bound socket until stop() is called.
    void listen();

    // Closes event streams, then stops serving. Safe to call from another
    // thread, including before listen() starts.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chronos::paper_trading
