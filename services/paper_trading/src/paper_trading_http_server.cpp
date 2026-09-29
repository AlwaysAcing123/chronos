#include "chronos/paper_trading/paper_trading_http_server.hpp"

#include "chronos/paper_trading/paper_trading_http_contract.hpp"

#include <httplib.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace chronos::paper_trading {
namespace {

// The raw query component. The library's own parameter map merges
// duplicates and form bodies leniently, so the JSON contract parses this
// text itself.
std::string raw_query(const httplib::Request& request) {
    const std::size_t question = request.target.find('?');
    return question == std::string::npos
        ? std::string()
        : request.target.substr(question + 1);
}

HttpRequest transport_independent(const httplib::Request& request) {
    return HttpRequest{
        .method = request.method,
        .path = request.path,
        .query = raw_query(request),
        .content_type = request.get_header_value("Content-Type"),
        .body = request.body,
    };
}

void set_common_headers(httplib::Response& response) {
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("X-Chronos-Trading-Mode", "paper");
}

void write_response(const HttpResponse& source, httplib::Response& target) {
    target.status = source.status;
    for (const HttpHeader& header : source.headers) {
        target.set_header(header.name, header.value);
    }
    set_common_headers(target);
    target.set_content(source.body, "application/json");
}

// Streams change notices to one browser. The provider blocks in next() on a
// reserved worker thread; a failed write means the browser left, and closing
// the notifier ends the stream at shutdown.
void stream_notices(
    market_data::MarketDataSubscription subscription,
    httplib::Response& response
) {
    struct Stream {
        market_data::MarketDataSubscription subscription;
        bool preamble_sent{false};
    };
    auto stream = std::make_shared<Stream>(Stream{
        .subscription = std::move(subscription),
        .preamble_sent = false,
    });
    response.status = 200;
    set_common_headers(response);
    response.set_chunked_content_provider(
        "text/event-stream",
        [stream](std::size_t, httplib::DataSink& sink) {
            if (!stream->preamble_sent) {
                stream->preamble_sent = true;
                return sink.write(
                    kEventStreamPreamble.data(),
                    kEventStreamPreamble.size()
                );
            }
            const std::optional<std::string> text =
                event_stream_text(stream->subscription.next());
            if (!text) {
                sink.done();
                return true;
            }
            return sink.write(text->data(), text->size());
        }
    );
}

// The HTTP library rejects some requests before routing, such as malformed
// HTTP or an oversized body. Those responses get the same JSON error shape.
std::string_view transport_error_code(int status) {
    switch (status) {
        case 400:
            return "malformed_http_request";
        case 404:
            return "route_not_found";
        case 405:
            return "method_not_allowed";
        case 408:
            return "request_timeout";
        case 413:
            return "payload_too_large";
        case 414:
            return "uri_too_long";
        case 431:
            return "request_headers_too_large";
        default:
            return "http_error";
    }
}

}  // namespace

struct PaperTradingHttpServer::Impl {
    Impl(
        PaperTradingHttpApi& served_api,
        HttpServerOptions server_options,
        DiagnosticLog access_log
    )
        : api(served_api),
          options(std::move(server_options)) {
        if (options.worker_threads == 0) {
            throw std::invalid_argument("HTTP server needs a worker thread");
        }
        const std::size_t worker_threads =
            options.worker_threads + api.event_stream_capacity();
        server.new_task_queue = [worker_threads] {
            return new httplib::ThreadPool(worker_threads);
        };
        server.set_payload_max_length(options.maximum_request_body_bytes);
        server.set_read_timeout(options.read_timeout);
        server.set_write_timeout(options.write_timeout);
        // The library defaults to SO_REUSEPORT, which lets a second server
        // process bind the same port and silently share its requests.
        // SO_REUSEADDR still permits a prompt restart after TIME_WAIT.
        server.set_socket_options([](auto socket) {
            httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1);
        });

        // Every method and path reaches the transport-independent router,
        // which owns 404 and 405 decisions; only a GET of the event-stream
        // route is streamed instead.
        const httplib::Server::Handler dispatch =
            [this](
                const httplib::Request& request,
                httplib::Response& response
            ) {
                const HttpRequest converted = transport_independent(request);
                if (!PaperTradingHttpApi::is_event_stream_request(converted)) {
                    write_response(api.handle(converted), response);
                    return;
                }
                auto opened = api.open_event_stream(converted);
                if (auto* rejected = std::get_if<HttpResponse>(&opened)) {
                    write_response(*rejected, response);
                    return;
                }
                stream_notices(
                    std::get<market_data::MarketDataSubscription>(
                        std::move(opened)
                    ),
                    response
                );
            };
        server.Get(".*", dispatch);
        server.Post(".*", dispatch);
        server.Put(".*", dispatch);
        server.Patch(".*", dispatch);
        server.Delete(".*", dispatch);
        server.Options(".*", dispatch);

        // Called for every status of 400 or more, including the router's own
        // JSON errors, so only library-generated empty bodies are replaced.
        server.set_error_handler(
            [](const httplib::Request&, httplib::Response& response) {
                if (response.body.empty()) {
                    write_response(
                        render_error(
                            response.status,
                            transport_error_code(response.status),
                            httplib::status_message(response.status)
                        ),
                        response
                    );
                }
            }
        );
        server.set_exception_handler(
            [](const httplib::Request&,
               httplib::Response& response,
               const std::exception_ptr&) {
                write_response(
                    render_error(
                        500,
                        "internal_error",
                        "the request could not be processed"
                    ),
                    response
                );
            }
        );
        server.set_logger(
            [access_log = std::move(access_log)](
                const httplib::Request& request,
                const httplib::Response& response
            ) {
                access_log(
                    request.method + " " + request.path + " " +
                    std::to_string(response.status)
                );
            }
        );
    }

    PaperTradingHttpApi& api;
    HttpServerOptions options;
    httplib::Server server;
    std::atomic<bool> stop_requested{false};
};

PaperTradingHttpServer::PaperTradingHttpServer(
    PaperTradingHttpApi& api,
    HttpServerOptions options,
    DiagnosticLog access_log
)
    : impl_(std::make_unique<Impl>(
          api,
          std::move(options),
          std::move(access_log)
      )) {}

PaperTradingHttpServer::~PaperTradingHttpServer() = default;

int PaperTradingHttpServer::bind() {
    const HttpServerOptions& options = impl_->options;
    if (options.port == 0) {
        const int port = impl_->server.bind_to_any_port(options.host);
        if (port < 0) {
            throw std::runtime_error(
                "could not bind an ephemeral port on " + options.host
            );
        }
        return port;
    }
    if (!impl_->server.bind_to_port(options.host, options.port)) {
        throw std::runtime_error(
            "could not bind " + options.host + ":" +
            std::to_string(options.port)
        );
    }
    return options.port;
}

void PaperTradingHttpServer::listen() {
    if (!impl_->server.listen_after_bind() && !impl_->stop_requested) {
        throw std::runtime_error("HTTP server stopped after a socket failure");
    }
}

void PaperTradingHttpServer::stop() {
    impl_->stop_requested = true;
    // Open streams hold worker threads that the server joins when it stops.
    impl_->api.close_event_streams();
    impl_->server.stop();
}

}  // namespace chronos::paper_trading
