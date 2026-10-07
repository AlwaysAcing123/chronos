#include "chronos/paper_trading/deployment_http.hpp"

#include "chronos/datasets/market_event_codec.hpp"
#include "chronos/features/top_of_book_features.hpp"
#include "chronos/paper_trading/paper_trading_http_contract.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chronos::paper_trading {
namespace {

using Json = nlohmann::json;
using ResponseJson = nlohmann::ordered_json;

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
// A day: longer than any sensible cadence, latency, or signal age.
constexpr std::int64_t kMaximumMilliseconds = 86'400'000;
constexpr std::size_t kDefaultPageLimit = 50;
constexpr std::size_t kMaximumPageLimit = 200;

HttpResponse json_response(int status, const ResponseJson& body) {
    return HttpResponse{.status = status, .headers = {}, .body = body.dump()};
}

HttpResponse error_response(int status, std::string_view code, std::string_view message,
                            std::string_view field = {}, std::string_view reason = {}) {
    ResponseJson error = ResponseJson::object();
    error["code"] = code;
    error["message"] = message;
    error["field"] = field.empty() ? ResponseJson(nullptr) : ResponseJson(field);
    error["reason"] = reason.empty() ? ResponseJson(nullptr) : ResponseJson(reason);
    ResponseJson body = ResponseJson::object();
    body["error"] = std::move(error);
    return json_response(status, body);
}

HttpResponse malformed(std::string_view field, std::string_view message) {
    return error_response(400, "malformed_request", message, field);
}

HttpResponse render(const DeploymentError& failure) {
    switch (failure.code) {
        case DeploymentErrorCode::InvalidRequest:
            return error_response(422, "invalid_deployment", failure.message, failure.field, failure.reason);
        case DeploymentErrorCode::DeploymentNotFound:
            return error_response(404, "deployment_not_found", failure.message);
        case DeploymentErrorCode::Conflict:
            return error_response(409, failure.reason, failure.message, failure.field);
        case DeploymentErrorCode::InvalidState:
            return error_response(409, "invalid_deployment_state", failure.message, failure.field, failure.reason);
        case DeploymentErrorCode::ModelUnavailable:
            return error_response(422, "model_unavailable", failure.message, failure.field, failure.reason);
        case DeploymentErrorCode::PersistenceUnavailable:
            return error_response(503, "persistence_unavailable", failure.message);
        case DeploymentErrorCode::Disabled:
            return error_response(409, "deployments_disabled", failure.message);
    }
    return error_response(500, "internal_error", "unknown deployment error");
}

std::string decimal_ms(std::int64_t nanoseconds) {
    // Created from whole milliseconds, so exact.
    return std::to_string(nanoseconds / kNanosecondsPerMillisecond);
}

std::string ratio_text(std::uint64_t ppb) {
    return features::Ratio::from_raw(static_cast<std::int64_t>(ppb)).to_string();
}

ResponseJson nullable_time(const std::optional<Timestamp>& at) {
    return at ? ResponseJson(format_timestamp(*at)) : ResponseJson(nullptr);
}

ResponseJson counters_json(const DeploymentRunCounters& run) {
    ResponseJson skipped = ResponseJson::object();
    for (const auto& [reason, count] : run.skipped) {
        skipped[reason] = std::to_string(count);
    }
    ResponseJson waits = ResponseJson::object();
    for (const auto& [reason, count] : run.waits) {
        waits[reason] = std::to_string(count);
    }
    ResponseJson json = ResponseJson::object();
    json["candidates"] = std::to_string(run.candidates);
    json["superseded"] = std::to_string(run.superseded);
    json["skipped"] = std::move(skipped);
    json["predictions"] = std::to_string(run.predictions);
    json["holds"] = std::to_string(run.holds);
    json["order_decisions"] = std::to_string(run.order_decisions);
    json["orders_recorded"] = std::to_string(run.orders_recorded);
    json["abandoned"] = std::to_string(run.abandoned);
    json["waits"] = std::move(waits);
    json["last_inference_us"] = std::to_string(run.last_inference_us);
    json["max_inference_us"] = std::to_string(run.max_inference_us);
    json["last_submission_us"] = std::to_string(run.last_submission_us);
    json["max_submission_us"] = std::to_string(run.max_submission_us);
    json["last_error"] = run.last_error ? ResponseJson(*run.last_error) : ResponseJson(nullptr);
    return json;
}

ResponseJson nullable_text(const std::optional<std::string>& text) {
    return text ? ResponseJson(*text) : ResponseJson(nullptr);
}

template <typename Integer>
ResponseJson nullable_integer(const std::optional<Integer>& value) {
    return value ? ResponseJson(std::to_string(*value)) : ResponseJson(nullptr);
}

ResponseJson outcome_json(const std::optional<DeploymentRuntimeOutcome>& outcome) {
    if (!outcome) {
        return nullptr;
    }
    ResponseJson json = ResponseJson::object();
    json["code"] = outcome->code;
    json["ingest_sequence"] = nullable_integer(outcome->ingest_sequence);
    json["observation_received_at"] = nullable_time(outcome->observation_receive);
    json["decision_sequence"] = nullable_integer(outcome->decision_sequence);
    json["at_market"] = nullable_time(outcome->at_market);
    json["at_wall"] = format_timestamp(outcome->at_wall);
    return json;
}

ResponseJson activity_json(const std::optional<DeploymentActivity>& activity) {
    if (!activity) {
        return nullptr;
    }
    ResponseJson json = ResponseJson::object();
    json["last_candidate"] = outcome_json(activity->last_candidate);
    json["last_submission"] = outcome_json(activity->last_submission);
    if (activity->pending) {
        ResponseJson pending = ResponseJson::object();
        pending["decision_sequence"] = std::to_string(activity->pending->decision_sequence);
        pending["observation_received_at"] = format_timestamp(activity->pending->observation_receive);
        pending["not_before_received_at"] = format_timestamp(activity->pending->not_before_receive);
        pending["expires_at"] = format_timestamp(activity->pending->expires_at_market);
        json["pending"] = std::move(pending);
    } else {
        json["pending"] = nullptr;
    }
    return json;
}

ResponseJson market_json(const std::optional<DeploymentMarketCondition>& market) {
    if (!market) {
        return nullptr;
    }
    const DeploymentQuoteCondition& quote = market->quote;
    ResponseJson quote_json = ResponseJson::object();
    quote_json["available"] = quote.available;
    quote_json["unavailable_reason"] = nullable_text(quote.unavailable_reason);
    quote_json["ingest_sequence"] = nullable_integer(quote.ingest_sequence);
    quote_json["received_at"] = nullable_time(quote.received_at);
    quote_json["age_ns"] = nullable_integer(quote.age_ns);
    quote_json["maximum_age_ns"] = std::to_string(quote.maximum_age_ns);
    quote_json["stale"] = quote.stale;
    quote_json["suspect"] = quote.suspect;
    ResponseJson json = ResponseJson::object();
    json["market_time"] = nullable_time(market->market_time);
    json["source_state"] = market->source_state
        ? ResponseJson(std::string(market_data::state_name(*market->source_state)))
        : ResponseJson(nullptr);
    json["trading_status"] = market->trading_status
        ? ResponseJson(std::string(datasets::trading_status_name(*market->trading_status)))
        : ResponseJson(nullptr);
    json["quote"] = std::move(quote_json);
    return json;
}

ResponseJson condition_json(const DeploymentCondition& condition) {
    ResponseJson json = ResponseJson::object();
    json["code"] = condition.code;
    json["reason"] = nullable_text(condition.reason);
    json["observation_received_at"] = nullable_time(condition.observation_receive);
    json["training_cutoff"] = nullable_time(condition.training_cutoff);
    json["next_eligible_received_at"] = nullable_time(condition.next_eligible_receive);
    json["not_before_received_at"] = nullable_time(condition.not_before_receive);
    json["expires_at"] = nullable_time(condition.expires_at);
    json["decision_sequence"] = nullable_integer(condition.decision_sequence);
    json["observed_at_market"] = nullable_time(condition.observed_at_market);
    json["observed_at_wall"] = nullable_time(condition.observed_at_wall);
    return json;
}

// A market-clock duration from `from` to `to`, exact, or why there is none.
// A negative or overflowing difference is reported, never clamped.
ResponseJson market_duration(std::optional<Timestamp> from, std::optional<Timestamp> to, std::string_view missing) {
    ResponseJson json = ResponseJson::object();
    json["clock"] = "market";
    if (!from || !to) {
        json["unavailable"] = missing;
        return json;
    }
    std::int64_t difference = 0;
    if (__builtin_sub_overflow(to->nanoseconds(), from->nanoseconds(), &difference)) {
        json["unavailable"] = "overflow";
    } else if (difference < 0) {
        json["unavailable"] = "inconsistent_clock";
    } else {
        json["nanoseconds"] = std::to_string(difference);
    }
    return json;
}

ResponseJson decision_timing_json(const DeploymentDecisionRecord& record) {
    const Timestamp signal = record.decision.observation_receive;
    const bool ordered = record.client_order_id.has_value();
    const std::string_view unresolved = !ordered ? "no_order"
        : record.state == DecisionState::Pending ? "not_resolved"
        : "no_execution";
    ResponseJson json = ResponseJson::object();
    json["signal_to_decision"] = market_duration(signal, record.decision.decided_at_market, "unavailable");
    json["execution_quote_offset"] = market_duration(
        signal, record.execution_receive, unresolved
    );
    json["signal_to_resolution"] = market_duration(
        signal, record.resolved_at_market, !ordered ? "no_order" : "not_resolved"
    );
    json["signal_to_fill"] = market_duration(
        signal,
        record.fill ? std::optional(record.fill->last_filled_at) : std::nullopt,
        !ordered ? "no_order" : record.state == DecisionState::Pending ? "not_resolved" : "no_fill"
    );
    return json;
}

ResponseJson fill_json(const std::optional<DecisionFill>& fill) {
    if (!fill) {
        return nullptr;
    }
    ResponseJson json = ResponseJson::object();
    json["count"] = std::to_string(fill->count);
    json["quantity"] = fill->quantity.to_string();
    json["notional"] = fill->notional.to_string();
    json["fees"] = fill->fees.to_string();
    json["price"] = fill->price ? ResponseJson(fill->price->to_string()) : ResponseJson(nullptr);
    json["first_filled_at"] = format_timestamp(fill->first_filled_at);
    json["last_filled_at"] = format_timestamp(fill->last_filled_at);
    return json;
}

ResponseJson deployment_json(const DeploymentStatus& status) {
    const ModelDeploymentRecord& record = status.deployment;
    const ModelDeploymentConfig& config = record.config;
    ResponseJson model = ResponseJson::object();
    model["experiment_id"] = config.experiment_id;
    model["name"] = config.model_name;
    model["instrument"] = config.instrument.value();
    model["parameters_sha256"] = config.parameters_sha256;
    model["feature_set"] = std::string(kDeploymentModelFeatureSet);
    model["training_cutoff"] = format_timestamp(config.training_cutoff);
    ResponseJson policy = ResponseJson::object();
    policy["name"] = std::string(kDeploymentPolicy);
    policy["threshold"] = ratio_text(config.threshold_ppb);
    policy["target_quantity"] = config.target_quantity.to_string();
    ResponseJson timing = ResponseJson::object();
    timing["decision_interval_ms"] = decimal_ms(config.decision_interval_ns);
    timing["latency_ms"] = decimal_ms(config.latency_ns);
    timing["max_signal_age_ms"] = decimal_ms(config.max_signal_age_ns);
    ResponseJson limits = ResponseJson::object();
    limits["max_orders_per_run"] = std::to_string(config.max_orders_per_run);
    limits["max_consecutive_rejections"] = std::to_string(config.max_consecutive_rejections);
    limits["orders_this_run"] = std::to_string(record.orders_this_run);
    limits["consecutive_rejections"] = std::to_string(record.consecutive_rejections);
    ResponseJson watermark = nullptr;
    if (record.watermark_receive) {
        watermark = ResponseJson::object();
        watermark["received_at"] = format_timestamp(*record.watermark_receive);
        watermark["ingest_sequence"] = std::to_string(*record.watermark_ingest_sequence);
    }
    ResponseJson json = ResponseJson::object();
    json["trading_mode"] = "paper";
    json["deployment_id"] = config.deployment_id;
    json["account_id"] = config.account_id;
    json["creation_sequence"] = std::to_string(record.creation_sequence);
    json["state"] = std::string(deployment_state_name(record.state));
    json["state_reason"] = record.state_reason;
    json["state_changed_at"] = record.state_changed_at;
    json["created_at"] = record.created_at;
    json["active"] = status.active;
    json["run_generation"] = std::to_string(record.run_generation);
    json["stream_id"] = config.stream.value();
    json["starting_cash"] = config.starting_cash.to_string();
    json["model"] = std::move(model);
    json["policy"] = std::move(policy);
    json["timing"] = std::move(timing);
    json["limits"] = std::move(limits);
    json["watermark"] = std::move(watermark);
    json["next_decision_sequence"] = std::to_string(record.next_decision_sequence);
    json["pending_decision"] = record.pending_decision
        ? ResponseJson(std::to_string(*record.pending_decision))
        : ResponseJson(nullptr);
    json["version"] = std::to_string(record.version);
    json["run"] = status.run ? counters_json(*status.run) : ResponseJson(nullptr);
    json["activity"] = activity_json(status.activity);
    json["market"] = market_json(status.market);
    json["condition"] = condition_json(status.condition);
    return json;
}

ResponseJson decision_json(const DeploymentDecisionRecord& record) {
    const NewDeploymentDecision& decision = record.decision;
    ResponseJson observation = ResponseJson::object();
    observation["stream_id"] = decision.stream.value();
    observation["instrument"] = decision.instrument.value();
    observation["ingest_sequence"] = std::to_string(decision.observation_ingest_sequence);
    observation["event_at"] = format_timestamp(decision.observation_event);
    observation["received_at"] = format_timestamp(decision.observation_receive);
    ResponseJson inputs = ResponseJson::array();
    for (std::int64_t value : decision.inputs) {
        inputs.push_back(std::to_string(value));
    }
    ResponseJson prediction = ResponseJson::object();
    prediction["class"] = decision.predicted_class;
    prediction["probabilities"] = ResponseJson(decision.probabilities);
    prediction["confidence"] = ratio_text(decision.confidence_ppb);
    ResponseJson order = nullptr;
    if (record.client_order_id) {
        order = ResponseJson::object();
        order["client_order_id"] = *record.client_order_id;
        order["side"] = *decision.side == Side::Buy ? "buy" : "sell";
        order["quantity"] = decision.quantity->to_string();
        order["not_before_received_at"] = nullable_time(record.not_before_receive);
        order["expires_at"] = nullable_time(record.expires_at_market);
    }
    ResponseJson execution = nullptr;
    if (record.execution_ingest_sequence) {
        execution = ResponseJson::object();
        execution["ingest_sequence"] = std::to_string(*record.execution_ingest_sequence);
        execution["received_at"] = nullable_time(record.execution_receive);
    }
    ResponseJson json = ResponseJson::object();
    json["decision_sequence"] = std::to_string(record.decision_sequence);
    json["run_generation"] = std::to_string(decision.run_generation);
    json["state"] = std::string(decision_state_name(record.state));
    json["observation"] = std::move(observation);
    json["decided_at"] = format_timestamp(decision.decided_at_market);
    json["inputs"] = std::move(inputs);
    json["prediction"] = std::move(prediction);
    json["position_before"] = decision.position_before.to_string();
    json["target_quantity"] = decision.target ? ResponseJson(decision.target->to_string()) : ResponseJson(nullptr);
    json["policy_reason"] = decision.policy_reason;
    json["order"] = std::move(order);
    json["resolution"] = record.resolution ? ResponseJson(*record.resolution) : ResponseJson(nullptr);
    json["execution"] = std::move(execution);
    json["resolved_at"] = nullable_time(record.resolved_at_market);
    json["fill"] = fill_json(record.fill);
    json["timing"] = decision_timing_json(record);
    json["created_at"] = record.created_at;
    return json;
}

// ---------------------------------------------------------------------------
// Requests

// `?limit=N&cursor=<prefix><sequence>`, each at most once.
struct PageQuery {
    std::size_t limit{kDefaultPageLimit};
    std::optional<std::int64_t> before;
};

std::optional<std::uint64_t> canonical_unsigned(std::string_view text) {
    if (text.empty() || text.size() > 19 || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::variant<PageQuery, HttpResponse> parse_page_query(std::string_view query, std::string_view cursor_prefix) {
    PageQuery page;
    if (query.ends_with('&')) {
        return malformed("query", "a query parameter needs a value");
    }
    std::set<std::string> seen;
    std::size_t start = 0;
    while (start < query.size()) {
        const std::size_t end = std::min(query.find('&', start), query.size());
        const std::string_view pair = query.substr(start, end - start);
        start = end + 1;
        const std::size_t equals = pair.find('=');
        if (equals == std::string_view::npos) {
            return malformed("query", "a query parameter needs a value");
        }
        const std::string name(pair.substr(0, equals));
        const std::string_view value = pair.substr(equals + 1);
        if (!seen.insert(name).second) {
            return malformed(name, "a query parameter may appear only once");
        }
        if (name == "limit") {
            const auto limit = canonical_unsigned(value);
            if (!limit || *limit == 0 || *limit > kMaximumPageLimit) {
                return error_response(400, "invalid_page_limit", "limit must be between 1 and 200", "limit");
            }
            page.limit = static_cast<std::size_t>(*limit);
        } else if (name == "cursor") {
            const auto sequence = value.starts_with(cursor_prefix)
                ? canonical_unsigned(value.substr(cursor_prefix.size()))
                : std::nullopt;
            if (!sequence || *sequence == 0 ||
                *sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return error_response(400, "invalid_cursor", "cursor is not a cursor of this list", "cursor");
            }
            page.before = static_cast<std::int64_t>(*sequence);
        } else {
            return malformed(name, "unexpected query parameter");
        }
    }
    return page;
}

std::optional<Json> strict_object(std::string_view body, std::string& duplicate) {
    std::vector<std::set<std::string>> keys;
    const Json::parser_callback_t callback = [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) {
            keys.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            keys.pop_back();
        } else if (event == Json::parse_event_t::key && !keys.empty() &&
                   !keys.back().insert(parsed.get<std::string>()).second && duplicate.empty()) {
            duplicate = parsed.get<std::string>();
        }
        return true;
    };
    try {
        Json parsed = Json::parse(body, callback);
        if (!parsed.is_object()) {
            return std::nullopt;
        }
        return parsed;
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

template <typename Decimal>
std::optional<Decimal> decimal_of(const std::string& text) {
    try {
        return Decimal::parse(text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

constexpr std::string_view kCreateFields[] = {
    "account_id",
    "decision_interval_ms",
    "deployment_id",
    "experiment_id",
    "instrument",
    "latency_ms",
    "max_consecutive_rejections",
    "max_orders_per_run",
    "max_signal_age_ms",
    "model",
    "starting_cash",
    "target_quantity",
    "threshold",
};

std::variant<HttpResponse, std::monostate> require_empty_object(const HttpRequest& request) {
    if (auto failure = parse_empty_object_request(request.body)) {
        return render_malformed_request(*failure);
    }
    return std::monostate{};
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// The same rule as every other JSON route: application/json, optionally
// with an explicit UTF-8 charset, so a browser cannot post cross-origin
// without a preflight this server never approves.
bool is_json(std::string_view content_type) {
    std::string lowered(content_type);
    for (char& character : lowered) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    const std::string_view value(lowered);
    const std::size_t separator = value.find(';');
    if (trimmed(value.substr(0, separator)) != "application/json") {
        return false;
    }
    return separator == std::string_view::npos || trimmed(value.substr(separator + 1)) == "charset=utf-8";
}

std::optional<HttpResponse> require_post_json(const HttpRequest& request, bool empty_body) {
    if (request.method != "POST") {
        HttpResponse response = error_response(405, "method_not_allowed", "this route allows POST");
        response.headers.push_back(HttpHeader{.name = "Allow", .value = "POST"});
        return response;
    }
    if (!request.query.empty()) {
        return malformed("query", "this route takes no query parameters");
    }
    if (!is_json(request.content_type)) {
        return error_response(415, "unsupported_media_type", "request Content-Type must be application/json");
    }
    if (empty_body) {
        auto empty = require_empty_object(request);
        if (auto* response = std::get_if<HttpResponse>(&empty)) {
            return *response;
        }
    }
    return std::nullopt;
}

std::optional<HttpResponse> require_get(const HttpRequest& request, std::string_view allowed = "GET") {
    if (request.method == "GET") {
        return std::nullopt;
    }
    HttpResponse response = error_response(405, "method_not_allowed", "this route allows " + std::string(allowed));
    response.headers.push_back(HttpHeader{.name = "Allow", .value = std::string(allowed)});
    return response;
}

HttpResponse list(DeploymentManager& manager, const HttpRequest& request) {
    auto query = parse_page_query(request.query, "d1.");
    if (auto* response = std::get_if<HttpResponse>(&query)) {
        return *response;
    }
    const PageQuery& page = std::get<PageQuery>(query);
    auto listed = manager.list(page.limit, page.before);
    if (const auto* failure = std::get_if<DeploymentError>(&listed)) {
        return render(*failure);
    }
    const auto& found = std::get<DeploymentListPage>(listed);
    ResponseJson deployments = ResponseJson::array();
    for (const auto& status : found.deployments) {
        deployments.push_back(deployment_json(status));
    }
    ResponseJson body = ResponseJson::object();
    body["enabled"] = manager.enabled();
    body["deployments"] = std::move(deployments);
    body["next_cursor"] = found.next_before_sequence
        ? ResponseJson("d1." + std::to_string(*found.next_before_sequence))
        : ResponseJson(nullptr);
    return json_response(200, body);
}

HttpResponse status_response(const DeploymentResult& result, int success = 200) {
    if (const auto* failure = std::get_if<DeploymentError>(&result)) {
        return render(*failure);
    }
    ResponseJson body = ResponseJson::object();
    body["deployment"] = deployment_json(std::get<DeploymentStatus>(result));
    return json_response(success, body);
}

}  // namespace

std::variant<DeploymentCreateRequest, MalformedDeploymentRequest> parse_deployment_create_request(std::string_view body) {
    const auto bad = [](std::string field, std::string message) {
        return MalformedDeploymentRequest{.field = std::move(field), .message = std::move(message)};
    };
    std::string duplicate;
    const auto object = strict_object(body, duplicate);
    if (!object) {
        return bad("", "the body must be a JSON object");
    }
    if (!duplicate.empty()) {
        return bad(duplicate, "a field may appear only once");
    }
    for (const auto& [key, value] : object->items()) {
        if (std::find(std::begin(kCreateFields), std::end(kCreateFields), key) == std::end(kCreateFields)) {
            return bad(key, "unexpected field");
        }
        if (!value.is_string()) {
            return bad(key, "every field is a string");
        }
    }
    for (std::string_view field : kCreateFields) {
        if (!object->contains(std::string(field))) {
            return bad(std::string(field), "missing field");
        }
    }
    const auto text = [&](std::string_view field) { return object->at(std::string(field)).get<std::string>(); };
    DeploymentCreateRequest request;
    request.deployment_id = text("deployment_id");
    request.account_id = text("account_id");
    request.experiment_id = text("experiment_id");
    request.model = text("model");
    request.instrument = text("instrument");
    const auto cash = decimal_of<Money>(text("starting_cash"));
    if (!cash) {
        return bad("starting_cash", "starting_cash must be a decimal string");
    }
    request.starting_cash = *cash;
    const auto target = decimal_of<Quantity>(text("target_quantity"));
    if (!target) {
        return bad("target_quantity", "target_quantity must be a decimal string");
    }
    request.target_quantity = *target;
    const auto threshold = decimal_of<features::Ratio>(text("threshold"));
    if (!threshold || threshold->raw() < 0) {
        return bad("threshold", "threshold must be a decimal string between 0 and 1");
    }
    request.threshold_ppb = static_cast<std::uint64_t>(threshold->raw());
    for (const auto& [field, target_ns] : {
             std::pair<std::string_view, std::int64_t*>{"decision_interval_ms", &request.decision_interval_ns},
             {"latency_ms", &request.latency_ns},
             {"max_signal_age_ms", &request.max_signal_age_ns},
         }) {
        const auto milliseconds = canonical_unsigned(text(field));
        if (!milliseconds || *milliseconds > static_cast<std::uint64_t>(kMaximumMilliseconds)) {
            return bad(std::string(field), std::string(field) + " must be whole milliseconds, at most a day");
        }
        *target_ns = static_cast<std::int64_t>(*milliseconds) * kNanosecondsPerMillisecond;
    }
    for (const auto& [field, target_count] : {
             std::pair<std::string_view, std::int32_t*>{"max_orders_per_run", &request.max_orders_per_run},
             {"max_consecutive_rejections", &request.max_consecutive_rejections},
         }) {
        const auto count = canonical_unsigned(text(field));
        if (!count || *count > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            return bad(std::string(field), std::string(field) + " must be a whole number");
        }
        *target_count = static_cast<std::int32_t>(*count);
    }
    return request;
}

HttpResponse deployment_route(
    DeploymentManager& manager,
    const HttpRequest& request,
    const std::vector<std::string_view>& parts,
    const std::function<void(std::string_view)>& log
) {
    if (parts.size() == 3) {
        if (request.method == "GET") {
            return list(manager, request);
        }
        if (request.method != "POST") {
            HttpResponse response = error_response(405, "method_not_allowed", "this route allows GET, POST");
            response.headers.push_back(HttpHeader{.name = "Allow", .value = "GET, POST"});
            return response;
        }
        if (auto rejected = require_post_json(request, false)) {
            return *rejected;
        }
        auto parsed = parse_deployment_create_request(request.body);
        if (const auto* failure = std::get_if<MalformedDeploymentRequest>(&parsed)) {
            return malformed(failure->field, failure->message);
        }
        auto created = manager.create(std::get<DeploymentCreateRequest>(parsed));
        if (const auto* failure = std::get_if<DeploymentError>(&created)) {
            if (failure->code == DeploymentErrorCode::PersistenceUnavailable) {
                log("create deployment: " + failure->message);
            }
            return render(*failure);
        }
        const auto& made = std::get<DeploymentCreated>(created);
        ResponseJson body = ResponseJson::object();
        body["deployment"] = deployment_json(made.status);
        body["created"] = made.created;
        return json_response(made.created ? 201 : 200, body);
    }
    if (parts.size() == 4 && parts[3] == "kill-switch") {
        if (auto rejected = require_post_json(request, true)) {
            return *rejected;
        }
        auto stopped = manager.kill_switch();
        if (const auto* failure = std::get_if<DeploymentError>(&stopped)) {
            return render(*failure);
        }
        ResponseJson deployments = ResponseJson::array();
        for (const auto& status : std::get<std::vector<DeploymentStatus>>(stopped)) {
            deployments.push_back(deployment_json(status));
        }
        ResponseJson body = ResponseJson::object();
        body["stopped"] = std::move(deployments);
        return json_response(200, body);
    }
    const std::string_view deployment_id = parts.size() >= 4 ? parts[3] : std::string_view{};
    if (parts.size() == 4) {
        if (auto rejected = require_get(request)) {
            return *rejected;
        }
        if (!request.query.empty()) {
            return malformed("query", "this route takes no query parameters");
        }
        return status_response(manager.get(deployment_id));
    }
    if (parts.size() == 5 && (parts[4] == "start" || parts[4] == "stop")) {
        if (auto rejected = require_post_json(request, true)) {
            return *rejected;
        }
        return status_response(parts[4] == "start" ? manager.start(deployment_id) : manager.stop(deployment_id));
    }
    if (parts.size() == 5 && parts[4] == "decisions") {
        if (auto rejected = require_get(request)) {
            return *rejected;
        }
        // Decision sequences are local to a deployment, not a global list.
        const std::string cursor_prefix = "x2." + std::string(deployment_id) + ".";
        auto query = parse_page_query(request.query, cursor_prefix);
        if (auto* response = std::get_if<HttpResponse>(&query)) {
            return *response;
        }
        const PageQuery& page = std::get<PageQuery>(query);
        auto found = manager.decisions(deployment_id, page.limit, page.before);
        if (const auto* failure = std::get_if<DeploymentError>(&found)) {
            return render(*failure);
        }
        const auto& decisions = std::get<DecisionPage>(found);
        ResponseJson rows = ResponseJson::array();
        for (const auto& decision : decisions.decisions) {
            rows.push_back(decision_json(decision));
        }
        ResponseJson body = ResponseJson::object();
        body["decisions"] = std::move(rows);
        body["next_cursor"] = decisions.next_before_sequence
            ? ResponseJson(cursor_prefix + std::to_string(*decisions.next_before_sequence))
            : ResponseJson(nullptr);
        return json_response(200, body);
    }
    return error_response(404, "route_not_found", "no such API route");
}

}  // namespace chronos::paper_trading
