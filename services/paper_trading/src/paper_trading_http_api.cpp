#include "chronos/paper_trading/paper_trading_http_api.hpp"

#include "chronos/paper_trading/deployment_http.hpp"
#include "chronos/paper_trading/paper_trading_http_contract.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::paper_trading {
namespace {

// Splits "/api/v1/accounts" into its segments. An empty segment, as in a
// trailing or doubled slash, makes the path unroutable rather than being
// silently normalized.
std::optional<std::vector<std::string_view>> path_segments(
    std::string_view path
) {
    if (path.empty() || path.front() != '/') {
        return std::nullopt;
    }
    std::vector<std::string_view> segments;
    std::size_t start = 1;
    while (true) {
        const std::size_t end = path.find('/', start);
        const std::string_view segment = path.substr(
            start,
            end == std::string_view::npos ? std::string_view::npos : end - start
        );
        if (segment.empty()) {
            return std::nullopt;
        }
        segments.push_back(segment);
        if (end == std::string_view::npos) {
            return segments;
        }
        start = end + 1;
    }
}

std::string lowercase_ascii(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return lowered;
}

std::string_view trim_spaces(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// JSON is UTF-8 by definition (RFC 8259), so the only accepted parameter is
// an explicit UTF-8 charset. Requiring this media type also means a browser
// cannot send a cross-origin order without a CORS preflight, which this
// server never approves.
bool is_json_media_type(std::string_view content_type) {
    const std::string lowered = lowercase_ascii(content_type);
    const std::string_view value(lowered);
    const std::size_t separator = value.find(';');
    if (trim_spaces(value.substr(0, separator)) != "application/json") {
        return false;
    }
    if (separator == std::string_view::npos) {
        return true;
    }
    return trim_spaces(value.substr(separator + 1)) == "charset=utf-8";
}

HttpResponse method_not_allowed(std::string_view allowed_methods) {
    HttpResponse response = render_error(
        405,
        "method_not_allowed",
        "this route allows " + std::string(allowed_methods)
    );
    response.headers.push_back(HttpHeader{
        .name = "Allow",
        .value = std::string(allowed_methods),
    });
    return response;
}

HttpResponse route_not_found() {
    return render_error(404, "route_not_found", "no such API route");
}

// Clients receive a generic persistence error; the libpq detail is logged.
template <typename Result>
const Result& log_unavailable(
    const Result& result,
    const DiagnosticLog& log,
    std::string_view operation
) {
    if (const auto* failure = std::get_if<PaperTradingError>(&result);
        failure != nullptr &&
        failure->code == PaperTradingErrorCode::PersistenceUnavailable) {
        log(std::string(operation) + ": " + failure->message);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Request preconditions, checked in this order: method, query, media type.

std::optional<HttpResponse> require_method(
    const HttpRequest& request,
    std::string_view method
) {
    if (request.method == method) {
        return std::nullopt;
    }
    return method_not_allowed(method);
}

std::optional<HttpResponse> require_no_query(const HttpRequest& request) {
    if (auto invalid = reject_query_parameters(request.query)) {
        return render_malformed_request(*invalid);
    }
    return std::nullopt;
}

std::optional<HttpResponse> require_json_body(const HttpRequest& request) {
    if (is_json_media_type(request.content_type)) {
        return std::nullopt;
    }
    return render_error(
        415,
        "unsupported_media_type",
        "request Content-Type must be application/json"
    );
}

// ---------------------------------------------------------------------------
// Routes

HttpResponse health(
    PaperTradingService& service,
    const market_data::MarketDataSource& source,
    const WorkingOrderMatcher* matcher,
    const DiagnosticLog& log,
    const HttpRequest& request
) {
    if (auto rejected = require_method(request, "GET")) {
        return *rejected;
    }
    if (auto rejected = require_no_query(request)) {
        return *rejected;
    }
    HttpResponse response = render_health(
        service.health(),
        source.status(),
        matcher ? std::optional<WorkingOrderMatcherStatus>(matcher->status())
                : std::nullopt
    );
    if (response.status != 200) {
        log("health check: PostgreSQL is unavailable");
    }
    return response;
}

HttpResponse create_account(
    PaperTradingService& service,
    const DiagnosticLog& log,
    const HttpRequest& request
) {
    if (auto rejected = require_method(request, "POST")) {
        return *rejected;
    }
    if (auto rejected = require_no_query(request)) {
        return *rejected;
    }
    if (auto rejected = require_json_body(request)) {
        return *rejected;
    }
    auto parsed = parse_create_account_request(request.body);
    if (const auto* malformed = std::get_if<MalformedRequest>(&parsed)) {
        return render_malformed_request(*malformed);
    }
    return render_account(
        log_unavailable(
            service.create_account(std::get<CreatePaperAccountRequest>(parsed)),
            log,
            "create account"
        ),
        201
    );
}

HttpResponse get_account(
    PaperTradingService& service,
    const DiagnosticLog& log,
    const HttpRequest& request,
    std::string_view account_id
) {
    if (auto rejected = require_method(request, "GET")) {
        return *rejected;
    }
    if (auto rejected = require_no_query(request)) {
        return *rejected;
    }
    return render_account(
        log_unavailable(service.get_account(account_id), log, "get account"),
        200
    );
}

// GET lists the durable order history; POST submits a new order.
HttpResponse orders(
    PaperTradingService& service,
    const DiagnosticLog& log,
    const HttpRequest& request,
    std::string_view account_id
) {
    if (request.method == "GET") {
        auto page = parse_history_query(request.query);
        if (const auto* malformed = std::get_if<MalformedRequest>(&page)) {
            return render_malformed_request(*malformed);
        }
        return render_order_history(log_unavailable(
            service.list_orders(account_id, std::get<HistoryPageRequest>(page)),
            log,
            "list orders"
        ));
    }
    if (request.method != "POST") {
        return method_not_allowed("GET, POST");
    }
    if (auto rejected = require_no_query(request)) {
        return *rejected;
    }
    if (auto rejected = require_json_body(request)) {
        return *rejected;
    }
    auto parsed = parse_order_request(std::string(account_id), request.body);
    if (const auto* malformed = std::get_if<MalformedRequest>(&parsed)) {
        return render_malformed_request(*malformed);
    }
    return render_order_submission(log_unavailable(
        service.submit_order(std::get<PaperOrderRequest>(parsed)),
        log,
        "submit order"
    ));
}

HttpResponse order(
    PaperTradingService& service,
    const DiagnosticLog& log,
    const HttpRequest& request,
    std::string_view account_id,
    std::string_view client_order_id
) {
    if (auto rejected = require_method(request, "GET")) {
        return *rejected;
    }
    if (auto rejected = require_no_query(request)) {
        return *rejected;
    }
    return render_order_record(log_unavailable(
        service.get_order(account_id, client_order_id),
        log,
        "get order"
    ));
}

// POST cancels the order's working remainder.
HttpResponse cancel_order(
    PaperTradingService& service,
    const DiagnosticLog& log,
    const HttpRequest& request,
    std::string_view account_id,
    std::string_view client_order_id
) {
    if (auto rejected = require_method(request, "POST")) {
        return *rejected;
    }
    if (auto rejected = require_no_query(request)) {
        return *rejected;
    }
    if (auto rejected = require_json_body(request)) {
        return *rejected;
    }
    auto parsed = parse_cancel_request(
        std::string(account_id),
        std::string(client_order_id),
        request.body
    );
    if (const auto* malformed = std::get_if<MalformedRequest>(&parsed)) {
        return render_malformed_request(*malformed);
    }
    return render_cancellation(log_unavailable(
        service.cancel_order(std::get<CancelPaperOrderRequest>(parsed)),
        log,
        "cancel order"
    ));
}

HttpResponse order_fills(
    PaperTradingService& service,
    const DiagnosticLog& log,
    const HttpRequest& request,
    std::string_view account_id,
    std::string_view client_order_id
) {
    if (auto rejected = require_method(request, "GET")) {
        return *rejected;
    }
    auto page = parse_forward_query(request.query, false);
    if (const auto* malformed = std::get_if<MalformedRequest>(&page)) {
        return render_malformed_request(*malformed);
    }
    return render_order_fills(log_unavailable(
        service.list_order_fills(
            account_id,
            client_order_id,
            std::get<ForwardPageRequest>(page)
        ),
        log,
        "list order fills"
    ));
}

HttpResponse order_events(
    PaperTradingService& service,
    const DiagnosticLog& log,
    const HttpRequest& request,
    std::string_view account_id,
    std::string_view client_order_id
) {
    if (auto rejected = require_method(request, "GET")) {
        return *rejected;
    }
    auto page = parse_forward_query(request.query, false);
    if (const auto* malformed = std::get_if<MalformedRequest>(&page)) {
        return render_malformed_request(*malformed);
    }
    return render_order_events(log_unavailable(
        service.list_order_events(
            account_id,
            client_order_id,
            std::get<ForwardPageRequest>(page)
        ),
        log,
        "list order events"
    ));
}

HttpResponse working_orders(
    PaperTradingService& service,
    const DiagnosticLog& log,
    const HttpRequest& request,
    std::string_view account_id
) {
    if (auto rejected = require_method(request, "GET")) {
        return *rejected;
    }
    auto page = parse_forward_query(request.query, true);
    if (const auto* malformed = std::get_if<MalformedRequest>(&page)) {
        return render_malformed_request(*malformed);
    }
    return render_working_orders(log_unavailable(
        service.list_working_orders(account_id, std::get<ForwardPageRequest>(page)),
        log,
        "list working orders"
    ));
}

HttpResponse portfolio(
    PaperTradingService& service,
    const DiagnosticLog& log,
    const HttpRequest& request,
    std::string_view account_id
) {
    if (auto rejected = require_method(request, "GET")) {
        return *rejected;
    }
    if (auto rejected = require_no_query(request)) {
        return *rejected;
    }
    return render_portfolio(log_unavailable(
        service.get_portfolio(account_id),
        log,
        "get portfolio"
    ));
}

HttpResponse fills(
    PaperTradingService& service,
    const DiagnosticLog& log,
    const HttpRequest& request,
    std::string_view account_id
) {
    if (auto rejected = require_method(request, "GET")) {
        return *rejected;
    }
    auto page = parse_history_query(request.query);
    if (const auto* malformed = std::get_if<MalformedRequest>(&page)) {
        return render_malformed_request(*malformed);
    }
    return render_fill_history(log_unavailable(
        service.list_fills(account_id, std::get<HistoryPageRequest>(page)),
        log,
        "list fills"
    ));
}

constexpr std::string_view kEventStreamPath = "/api/v1/market-data/events";

std::optional<market_data::ReplayCommand> replay_command(std::string_view name) {
    for (const auto command : {
             market_data::ReplayCommand::Start,
             market_data::ReplayCommand::Pause,
             market_data::ReplayCommand::Resume,
         }) {
        if (market_data::command_name(command) == name) {
            return command;
        }
    }
    return std::nullopt;
}

}  // namespace

PaperTradingHttpApi::PaperTradingHttpApi(
    PaperTradingService& service,
    market_data::MarketDataSource& source,
    market_data::MarketDataNotifier& notifier,
    DiagnosticLog log,
    const WorkingOrderMatcher* matcher,
    DeploymentManager* deployments
)
    : service_(service),
      source_(source),
      notifier_(notifier),
      log_(std::move(log)),
      matcher_(matcher),
      deployments_(deployments) {}

bool PaperTradingHttpApi::is_event_stream_request(const HttpRequest& request) {
    return request.method == "GET" && request.path == kEventStreamPath;
}

std::variant<HttpResponse, market_data::MarketDataSubscription>
PaperTradingHttpApi::open_event_stream(const HttpRequest& request) {
    if (auto rejected = require_method(request, "GET")) {
        return *rejected;
    }
    if (auto rejected = require_no_query(request)) {
        return *rejected;
    }
    auto subscription = notifier_.subscribe();
    if (const auto* error =
            std::get_if<market_data::MarketDataNotifier::SubscribeError>(
                &subscription
            )) {
        return render_event_stream_unavailable(*error);
    }
    return std::get<market_data::MarketDataSubscription>(std::move(subscription));
}

void PaperTradingHttpApi::close_event_streams() { notifier_.close(); }

std::size_t PaperTradingHttpApi::event_stream_capacity() const noexcept {
    return notifier_.maximum_subscribers();
}

MarketDataView PaperTradingHttpApi::market_data_view() const {
    // The revision is read first, so the view is at least as new as it.
    const std::uint64_t revision = notifier_.revision();
    return MarketDataView{
        .source = source_.status(),
        .market = service_.market_availability(),
        .revision = revision,
    };
}

HttpResponse PaperTradingHttpApi::market_data_route(
    const HttpRequest& request,
    const std::vector<std::string_view>& parts
) {
    if (parts.size() == 3) {
        if (auto rejected = require_method(request, "GET")) {
            return *rejected;
        }
        if (auto rejected = require_no_query(request)) {
            return *rejected;
        }
        return render_market_data(market_data_view());
    }
    if (parts.size() == 4 && parts[3] == "events") {
        if (auto rejected = require_method(request, "GET")) {
            return *rejected;
        }
        // Only a streaming transport can serve this route.
        log_("event stream requested through a non-streaming transport");
        return render_error(
            500,
            "internal_error",
            "the event stream is not available on this transport"
        );
    }
    const std::optional<market_data::ReplayCommand> command =
        parts.size() == 5 && parts[3] == "replay"
        ? replay_command(parts[4])
        : std::nullopt;
    if (!command) {
        return route_not_found();
    }
    if (auto rejected = require_method(request, "POST")) {
        return *rejected;
    }
    if (auto rejected = require_no_query(request)) {
        return *rejected;
    }
    if (auto rejected = require_json_body(request)) {
        return *rejected;
    }
    if (auto malformed = parse_empty_object_request(request.body)) {
        return render_malformed_request(*malformed);
    }
    const market_data::ReplayCommandResult result =
        source_.control_replay(*command);
    if (const auto* error = std::get_if<market_data::ReplayCommandError>(&result)) {
        return render_replay_command_error(
            *error,
            source_.status().description.mode
        );
    }
    return render_market_data(market_data_view());
}

HttpResponse PaperTradingHttpApi::handle(const HttpRequest& request) {
    try {
        return route(request);
    } catch (const std::exception& failure) {
        log_(
            "internal error handling " + request.method + " " + request.path +
            ": " + failure.what()
        );
    } catch (...) {
        log_(
            "internal error handling " + request.method + " " + request.path +
            ": unknown exception"
        );
    }
    return render_error(
        500,
        "internal_error",
        "the request could not be processed; if this request writes data, it "
        "may or may not have completed, so retry the same request with the "
        "same identifiers"
    );
}

HttpResponse PaperTradingHttpApi::route(const HttpRequest& request) {
    const auto segments = path_segments(request.path);
    if (!segments) {
        return route_not_found();
    }
    const auto& parts = *segments;
    if (parts.size() == 1 && parts[0] == "health") {
        return health(service_, source_, matcher_, log_, request);
    }
    if (parts.size() >= 3 && parts[0] == "api" && parts[1] == "v1" &&
        parts[2] == "market-data") {
        return market_data_route(request, parts);
    }
    if (deployments_ != nullptr && parts.size() >= 3 && parts[0] == "api" && parts[1] == "v1" &&
        parts[2] == "deployments") {
        return deployment_route(*deployments_, request, parts, log_);
    }

    const bool is_accounts_path = parts.size() >= 3 && parts[0] == "api" &&
        parts[1] == "v1" && parts[2] == "accounts";
    if (!is_accounts_path) {
        return route_not_found();
    }
    if (parts.size() == 3) {
        return create_account(service_, log_, request);
    }
    const std::string_view account_id = parts[3];
    if (parts.size() == 4) {
        return get_account(service_, log_, request, account_id);
    }
    if (parts.size() == 5 && parts[4] == "orders") {
        return orders(service_, log_, request, account_id);
    }
    if (parts.size() == 5 && parts[4] == "fills") {
        return fills(service_, log_, request, account_id);
    }
    if (parts.size() == 5 && parts[4] == "portfolio") {
        return portfolio(service_, log_, request, account_id);
    }
    if (parts.size() == 5 && parts[4] == "working-orders") {
        return working_orders(service_, log_, request, account_id);
    }
    if (parts.size() == 6 && parts[4] == "orders") {
        return order(service_, log_, request, account_id, parts[5]);
    }
    if (parts.size() == 7 && parts[4] == "orders") {
        const std::string_view client_order_id = parts[5];
        if (parts[6] == "cancel") {
            return cancel_order(service_, log_, request, account_id, client_order_id);
        }
        if (parts[6] == "fills") {
            return order_fills(service_, log_, request, account_id, client_order_id);
        }
        if (parts[6] == "events") {
            return order_events(service_, log_, request, account_id, client_order_id);
        }
    }
    return route_not_found();
}

}  // namespace chronos::paper_trading
