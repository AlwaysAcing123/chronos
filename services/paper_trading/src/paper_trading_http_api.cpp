#include "chronos/paper_trading/paper_trading_http_api.hpp"

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

HttpResponse method_not_allowed(std::string_view allowed_method) {
    HttpResponse response = render_error(
        405,
        "method_not_allowed",
        "this path supports " + std::string(allowed_method) + " only"
    );
    response.headers.push_back(HttpHeader{
        .name = "Allow",
        .value = std::string(allowed_method),
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

}  // namespace

PaperTradingHttpApi::PaperTradingHttpApi(
    PaperTradingService& service,
    DiagnosticLog log
)
    : service_(service),
      log_(std::move(log)) {}

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
    const bool is_accounts_path = parts.size() >= 3 && parts[0] == "api" &&
        parts[1] == "v1" && parts[2] == "accounts";

    if (parts.size() == 1 && parts[0] == "health") {
        if (request.method != "GET") {
            return method_not_allowed("GET");
        }
        const HttpResponse response = render_health(service_.health());
        if (response.status != 200) {
            log_("health check: PostgreSQL is unavailable");
        }
        return response;
    }

    if (is_accounts_path && parts.size() == 4) {
        if (request.method != "GET") {
            return method_not_allowed("GET");
        }
        return render_account(
            log_unavailable(
                service_.get_account(parts[3]),
                log_,
                "get account"
            ),
            200
        );
    }

    const bool is_create_account = is_accounts_path && parts.size() == 3;
    const bool is_submit_order = is_accounts_path && parts.size() == 5 &&
        parts[4] == "orders";
    if (!is_create_account && !is_submit_order) {
        return route_not_found();
    }
    if (request.method != "POST") {
        return method_not_allowed("POST");
    }
    if (!is_json_media_type(request.content_type)) {
        return render_error(
            415,
            "unsupported_media_type",
            "request Content-Type must be application/json"
        );
    }

    if (is_create_account) {
        auto parsed = parse_create_account_request(request.body);
        if (const auto* malformed = std::get_if<MalformedRequest>(&parsed)) {
            return render_malformed_request(*malformed);
        }
        return render_account(
            log_unavailable(
                service_.create_account(
                    std::get<CreatePaperAccountRequest>(parsed)
                ),
                log_,
                "create account"
            ),
            201
        );
    }

    auto parsed = parse_order_request(std::string(parts[3]), request.body);
    if (const auto* malformed = std::get_if<MalformedRequest>(&parsed)) {
        return render_malformed_request(*malformed);
    }
    return render_order_submission(log_unavailable(
        service_.submit_order(std::get<PaperOrderRequest>(parsed)),
        log_,
        "submit order"
    ));
}

}  // namespace chronos::paper_trading
