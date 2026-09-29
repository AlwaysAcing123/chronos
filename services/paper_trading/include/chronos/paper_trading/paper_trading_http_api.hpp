#pragma once

#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_data_source.hpp"
#include "chronos/paper_trading/http_message.hpp"
#include "chronos/paper_trading/paper_trading_http_contract.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"

#include <cstddef>
#include <functional>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::paper_trading {

// Receives diagnostic lines that must not be sent to clients, such as the
// details of an internal failure.
using DiagnosticLog = std::function<void(std::string_view line)>;

// Routes transport-independent HTTP requests to the paper-trading service.
//
//   GET  /health
//   POST /api/v1/accounts
//   GET  /api/v1/accounts/{account_id}
//   POST /api/v1/accounts/{account_id}/orders
//   GET  /api/v1/accounts/{account_id}/orders
//   GET  /api/v1/accounts/{account_id}/orders/{client_order_id}
//   GET  /api/v1/accounts/{account_id}/fills
//   GET  /api/v1/accounts/{account_id}/portfolio
//   GET  /api/v1/market-data
//   POST /api/v1/market-data/replay/{start,pause,resume}
//   GET  /api/v1/market-data/events            (Server-Sent Events)
//
// The router holds no trading logic: it checks the method, query string, and
// media type, parses the request through the JSON contract, calls the
// service or market-data source, and renders the typed result. Every
// dependency must outlive the router.
class PaperTradingHttpApi final {
public:
    PaperTradingHttpApi(
        PaperTradingService& service,
        market_data::MarketDataSource& source,
        market_data::MarketDataNotifier& notifier,
        DiagnosticLog log
    );

    // Never throws for request content. An unexpected exception becomes a
    // generic 500 response and its details go only to the diagnostic log.
    [[nodiscard]] HttpResponse handle(const HttpRequest& request);

    // A GET of the event-stream route, which a streaming transport serves
    // with open_event_stream() instead of handle().
    [[nodiscard]] static bool is_event_stream_request(const HttpRequest& request);

    // A subscription to stream, or the error response to send instead.
    [[nodiscard]] std::variant<HttpResponse, market_data::MarketDataSubscription>
    open_event_stream(const HttpRequest& request);

    // Ends every open event stream so the transport can shut down.
    void close_event_streams();

    // How many event streams may be open at once; each holds a transport
    // thread while it is open.
    [[nodiscard]] std::size_t event_stream_capacity() const noexcept;

private:
    [[nodiscard]] HttpResponse route(const HttpRequest& request);
    [[nodiscard]] HttpResponse market_data_route(
        const HttpRequest& request,
        const std::vector<std::string_view>& parts
    );
    [[nodiscard]] MarketDataView market_data_view() const;

    PaperTradingService& service_;
    market_data::MarketDataSource& source_;
    market_data::MarketDataNotifier& notifier_;
    DiagnosticLog log_;
};

}  // namespace chronos::paper_trading
