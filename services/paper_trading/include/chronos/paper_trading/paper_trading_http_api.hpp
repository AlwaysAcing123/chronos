#pragma once

#include "chronos/paper_trading/http_message.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"

#include <functional>
#include <string_view>

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
//
// The router holds no trading logic: it checks the method, query string, and
// media type, parses the request through the JSON contract, calls the
// service, and renders the typed result. `service` must outlive the router.
class PaperTradingHttpApi final {
public:
    PaperTradingHttpApi(PaperTradingService& service, DiagnosticLog log);

    // Never throws for request content. An unexpected exception becomes a
    // generic 500 response and its details go only to the diagnostic log.
    [[nodiscard]] HttpResponse handle(const HttpRequest& request);

private:
    [[nodiscard]] HttpResponse route(const HttpRequest& request);

    PaperTradingService& service_;
    DiagnosticLog log_;
};

}  // namespace chronos::paper_trading
