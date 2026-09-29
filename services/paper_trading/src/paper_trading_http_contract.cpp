#include "chronos/paper_trading/paper_trading_http_contract.hpp"

#include "chronos/paper_order_codes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::paper_trading {
namespace {

using RequestJson = nlohmann::json;
// Responses preserve field insertion order so documents read in a deliberate
// order; the order is part of neither the contract nor any comparison.
using ResponseJson = nlohmann::ordered_json;

// ---------------------------------------------------------------------------
// Request parsing

MalformedRequest malformed(
    MalformedRequestReason reason,
    std::string field,
    std::string message
) {
    return MalformedRequest{
        .reason = reason,
        .field = std::move(field),
        .message = std::move(message),
    };
}

// Keys already seen in one JSON object, with that object's field path.
struct ObjectKeys {
    std::string path_prefix;
    std::string last_key;
    std::set<std::string> keys;
};

// Duplicate keys are ambiguous: parsers disagree about which value wins, so a
// request such as {"quantity":"1","quantity":"1000"} is rejected outright.
std::variant<RequestJson, MalformedRequest> parse_json_object(
    std::string_view body
) {
    std::vector<ObjectKeys> open_objects;
    std::optional<std::string> duplicate_key;
    const RequestJson::parser_callback_t detect_duplicate_keys =
        [&](int, RequestJson::parse_event_t event, RequestJson& parsed) {
            if (event == RequestJson::parse_event_t::object_start) {
                open_objects.push_back(ObjectKeys{
                    .path_prefix = open_objects.empty()
                        ? std::string()
                        : open_objects.back().path_prefix +
                            open_objects.back().last_key + ".",
                    .last_key = {},
                    .keys = {},
                });
            } else if (event == RequestJson::parse_event_t::object_end) {
                open_objects.pop_back();
            } else if (event == RequestJson::parse_event_t::key) {
                ObjectKeys& object = open_objects.back();
                object.last_key = parsed.get_ref<const std::string&>();
                if (!object.keys.insert(object.last_key).second &&
                    !duplicate_key) {
                    duplicate_key = object.path_prefix + object.last_key;
                }
            }
            return true;
        };

    RequestJson document = RequestJson::parse(
        body.begin(),
        body.end(),
        detect_duplicate_keys,
        false
    );
    if (document.is_discarded()) {
        return malformed(
            MalformedRequestReason::InvalidJson,
            "",
            "request body is not valid UTF-8 JSON"
        );
    }
    if (duplicate_key) {
        return malformed(
            MalformedRequestReason::DuplicateField,
            *duplicate_key,
            *duplicate_key + " appears more than once"
        );
    }
    if (!document.is_object()) {
        return malformed(
            MalformedRequestReason::InvalidType,
            "",
            "request body must be a JSON object"
        );
    }
    return document;
}

enum class DecimalTextError {
    Invalid,
    TooManyFractionalDigits,
    OutOfRange,
};

bool is_ascii_digit(char character) {
    return character >= '0' && character <= '9';
}

// Accepts -?(0|[1-9][0-9]*)(\.[0-9]+)? with at most the type's fractional
// digits. Excess precision is rejected rather than rounded.
template <typename Decimal>
std::variant<Decimal, DecimalTextError> parse_decimal_text(
    std::string_view text
) {
    std::size_t index = 0;
    if (index < text.size() && text[index] == '-') {
        ++index;
    }
    const std::size_t whole_start = index;
    while (index < text.size() && is_ascii_digit(text[index])) {
        ++index;
    }
    const std::size_t whole_digits = index - whole_start;
    if (whole_digits == 0 || (whole_digits > 1 && text[whole_start] == '0')) {
        return DecimalTextError::Invalid;
    }

    std::size_t fractional_digits = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        const std::size_t fraction_start = index;
        while (index < text.size() && is_ascii_digit(text[index])) {
            ++index;
        }
        fractional_digits = index - fraction_start;
        if (fractional_digits == 0) {
            return DecimalTextError::Invalid;
        }
    }
    if (index != text.size()) {
        return DecimalTextError::Invalid;
    }
    if (fractional_digits > static_cast<std::size_t>(Decimal::kDecimalPlaces)) {
        return DecimalTextError::TooManyFractionalDigits;
    }

    try {
        return Decimal::parse(text);
    } catch (const std::overflow_error&) {
        return DecimalTextError::OutOfRange;
    }
}

template <typename Enum>
std::string allowed_names() {
    std::string names;
    for (const Enum value : declared_values<Enum>()) {
        if (!names.empty()) {
            names += ", ";
        }
        names += *canonical_name(value);
    }
    return names;
}

// Reads the fields of one JSON object. The first problem found is kept in a
// slot shared with nested readers, so a request reports one deterministic
// error and every accessor after it becomes a no-op.
class FieldReader final {
public:
    FieldReader(
        const RequestJson& object,
        std::string path_prefix,
        std::initializer_list<std::string_view> allowed_fields,
        std::optional<MalformedRequest>& error
    )
        : object_(object),
          prefix_(std::move(path_prefix)),
          error_(error) {
        const std::set<std::string_view> allowed(allowed_fields);
        for (const auto& item : object_.items()) {
            if (!allowed.contains(item.key())) {
                fail(
                    MalformedRequestReason::UnexpectedField,
                    item.key(),
                    path(item.key()) + " is not a recognized field"
                );
                return;
            }
        }
    }

    [[nodiscard]] bool contains(std::string_view name) const {
        return object_.contains(name);
    }

    [[nodiscard]] std::optional<std::string> string(std::string_view name) {
        const RequestJson* value = required(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_string()) {
            fail(
                MalformedRequestReason::InvalidType,
                name,
                path(name) + " must be a string"
            );
            return std::nullopt;
        }
        return value->get<std::string>();
    }

    template <typename Decimal>
    [[nodiscard]] std::optional<Decimal> decimal(std::string_view name) {
        const RequestJson* value = required(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_string()) {
            fail(
                MalformedRequestReason::InvalidType,
                name,
                path(name) + " must be a decimal string, not a JSON " +
                    value->type_name()
            );
            return std::nullopt;
        }
        auto parsed = parse_decimal_text<Decimal>(
            value->get_ref<const std::string&>()
        );
        if (const auto* decimal_value = std::get_if<Decimal>(&parsed)) {
            return *decimal_value;
        }
        switch (std::get<DecimalTextError>(parsed)) {
            case DecimalTextError::Invalid:
                fail(
                    MalformedRequestReason::InvalidDecimal,
                    name,
                    path(name) +
                        " must be a plain decimal string such as \"12.5\""
                );
                break;
            case DecimalTextError::TooManyFractionalDigits:
                fail(
                    MalformedRequestReason::TooManyFractionalDigits,
                    name,
                    path(name) + " supports at most " +
                        std::to_string(Decimal::kDecimalPlaces) +
                        " fractional digits"
                );
                break;
            case DecimalTextError::OutOfRange:
                fail(
                    MalformedRequestReason::DecimalOutOfRange,
                    name,
                    path(name) + " is outside the supported fixed-point range"
                );
                break;
        }
        return std::nullopt;
    }

    template <typename Enum>
    [[nodiscard]] std::optional<Enum> enumeration(std::string_view name) {
        const std::optional<std::string> text = string(name);
        if (!text) {
            return std::nullopt;
        }
        if (const auto value = from_canonical_name<Enum>(*text)) {
            return value;
        }
        fail(
            MalformedRequestReason::UnknownEnumValue,
            name,
            path(name) + " must be one of: " + allowed_names<Enum>()
        );
        return std::nullopt;
    }

    [[nodiscard]] std::optional<FieldReader> object(
        std::string_view name,
        std::initializer_list<std::string_view> allowed_fields
    ) {
        const RequestJson* value = required(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_object()) {
            fail(
                MalformedRequestReason::InvalidType,
                name,
                path(name) + " must be an object"
            );
            return std::nullopt;
        }
        return FieldReader(*value, path(name) + ".", allowed_fields, error_);
    }

private:
    const RequestJson* required(std::string_view name) {
        const auto position = object_.find(name);
        if (position == object_.end()) {
            fail(
                MalformedRequestReason::MissingField,
                name,
                path(name) + " is required"
            );
            return nullptr;
        }
        return &*position;
    }

    [[nodiscard]] std::string path(std::string_view name) const {
        return prefix_ + std::string(name);
    }

    void fail(
        MalformedRequestReason reason,
        std::string_view name,
        std::string message
    ) {
        if (!error_) {
            error_ = malformed(reason, path(name), std::move(message));
        }
    }

    const RequestJson& object_;
    std::string prefix_;
    std::optional<MalformedRequest>& error_;
};

struct QueryParameter {
    std::string name;
    std::string value;
};

std::optional<int> hex_digit(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return std::nullopt;
}

// Decodes %XX escapes. '+' is an ordinary character: this API does not use
// form encoding, and no parameter value legitimately contains a space.
std::optional<std::string> percent_decode(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '%') {
            decoded += text[index];
            continue;
        }
        if (text.size() - index < 3) {
            return std::nullopt;
        }
        const auto high = hex_digit(text[index + 1]);
        const auto low = hex_digit(text[index + 2]);
        if (!high || !low) {
            return std::nullopt;
        }
        decoded += static_cast<char>(*high * 16 + *low);
        index += 2;
    }
    return decoded;
}

MalformedRequest invalid_query(std::string message) {
    return malformed(
        MalformedRequestReason::InvalidQuery,
        "",
        std::move(message)
    );
}

// Parses name=value pairs separated by '&'. Every problem is an error:
// empty pairs, pairs without '=', bad escapes, names outside `allowed`, and
// repeated names. An unknown name is echoed only when it is a plain
// identifier, so arbitrary bytes never reach a JSON response.
std::variant<std::vector<QueryParameter>, MalformedRequest> parse_query(
    std::string_view query,
    std::initializer_list<std::string_view> allowed
) {
    std::vector<QueryParameter> parameters;
    if (query.empty()) {
        return parameters;
    }
    std::set<std::string> seen;
    while (true) {
        const std::size_t separator = query.find('&');
        const std::string_view pair = query.substr(0, separator);
        const std::size_t equals = pair.find('=');
        if (pair.empty() || equals == std::string_view::npos || equals == 0) {
            return invalid_query(
                "each query parameter must have the form name=value"
            );
        }
        auto name = percent_decode(pair.substr(0, equals));
        auto value = percent_decode(pair.substr(equals + 1));
        if (!name || !value) {
            return invalid_query(
                "query string contains an invalid percent escape"
            );
        }
        if (std::find(allowed.begin(), allowed.end(), *name) == allowed.end()) {
            if (!is_valid_identifier(*name)) {
                return malformed(
                    MalformedRequestReason::UnexpectedField,
                    "",
                    "query string contains an unrecognized parameter"
                );
            }
            return malformed(
                MalformedRequestReason::UnexpectedField,
                *name,
                *name + " is not a recognized query parameter"
            );
        }
        if (!seen.insert(*name).second) {
            return malformed(
                MalformedRequestReason::DuplicateField,
                *name,
                *name + " appears more than once"
            );
        }
        parameters.push_back(QueryParameter{
            .name = std::move(*name),
            .value = std::move(*value),
        });
        if (separator == std::string_view::npos) {
            return parameters;
        }
        query.remove_prefix(separator + 1);
    }
}

// A canonical base-10 integer: no sign, no leading zeros. The service owns
// the allowed range.
std::optional<std::size_t> parse_limit(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0') ||
        !std::all_of(text.begin(), text.end(), is_ascii_digit)) {
        return std::nullopt;
    }
    std::size_t limit = 0;
    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        limit
    );
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return limit;
}

// ---------------------------------------------------------------------------
// Response rendering

template <typename Enum>
std::string name_of(Enum value) {
    const std::optional<std::string_view> name = canonical_name(value);
    if (!name) {
        throw std::logic_error("cannot render an undeclared enum value");
    }
    return std::string(*name);
}

std::string_view reason_name(MalformedRequestReason reason) {
    switch (reason) {
        case MalformedRequestReason::InvalidJson:
            return "invalid_json";
        case MalformedRequestReason::InvalidType:
            return "invalid_type";
        case MalformedRequestReason::MissingField:
            return "missing_field";
        case MalformedRequestReason::UnexpectedField:
            return "unexpected_field";
        case MalformedRequestReason::DuplicateField:
            return "duplicate_field";
        case MalformedRequestReason::InvalidDecimal:
            return "invalid_decimal";
        case MalformedRequestReason::TooManyFractionalDigits:
            return "too_many_fractional_digits";
        case MalformedRequestReason::DecimalOutOfRange:
            return "decimal_out_of_range";
        case MalformedRequestReason::UnknownEnumValue:
            return "unknown_enum_value";
        case MalformedRequestReason::InvalidQuery:
            return "invalid_query";
        case MalformedRequestReason::InvalidLimit:
            return "invalid_limit";
    }
    throw std::logic_error("unknown malformed-request reason");
}

std::string_view valuation_reason(PortfolioValuationErrorCode code) {
    switch (code) {
        case PortfolioValuationErrorCode::InvalidMarketState:
            return "invalid_market_state";
        case PortfolioValuationErrorCode::MarketInstrumentMismatch:
            return "market_instrument_mismatch";
        case PortfolioValuationErrorCode::SuspectMarketState:
            return "suspect_market_state";
        case PortfolioValuationErrorCode::MissingBestBid:
            return "missing_best_bid";
        case PortfolioValuationErrorCode::StaleMarketState:
            return "stale_market_state";
        case PortfolioValuationErrorCode::MarketTimelineMismatch:
            return "incompatible_market_timeline";
        case PortfolioValuationErrorCode::NonpositiveStartingCash:
            return "nonpositive_starting_cash";
        case PortfolioValuationErrorCode::ArithmeticOverflow:
            return "arithmetic_overflow";
        case PortfolioValuationErrorCode::AccountingIdentityFailed:
            return "accounting_identity_failed";
    }
    throw std::logic_error("unknown portfolio valuation error");
}

std::string_view market_state_reason(MarketStateUnavailableCode code) {
    switch (code) {
        case MarketStateUnavailableCode::StreamInterrupted:
            return "stream_interrupted";
        case MarketStateUnavailableCode::NoTopOfBook:
            return "no_top_of_book";
    }
    throw std::logic_error("unknown market-state unavailability");
}

ResponseJson nullable(std::optional<std::string> value) {
    return value ? ResponseJson(std::move(*value)) : ResponseJson(nullptr);
}

ResponseJson error_json(
    std::string_view code,
    std::string_view message,
    std::optional<std::string> field = std::nullopt,
    std::optional<std::string> reason = std::nullopt
) {
    ResponseJson error = ResponseJson::object();
    error["code"] = code;
    error["message"] = message;
    error["field"] = nullable(std::move(field));
    error["reason"] = nullable(std::move(reason));
    return error;
}

HttpResponse json_response(int status, const ResponseJson& body) {
    return HttpResponse{
        .status = status,
        .headers = {},
        .body = body.dump(),
    };
}

HttpResponse error_response(
    int status,
    std::string_view code,
    std::string_view message,
    std::optional<std::string> field = std::nullopt,
    std::optional<std::string> reason = std::nullopt
) {
    ResponseJson body = ResponseJson::object();
    body["error"] = error_json(
        code,
        message,
        std::move(field),
        std::move(reason)
    );
    return json_response(status, body);
}

std::optional<std::string> nonempty(const std::string& value) {
    return value.empty() ? std::nullopt : std::optional<std::string>(value);
}

// Domain rejections name internal fields such as "market.header.quality".
// The API reports a field only when it names part of the client's request.
std::optional<std::string> request_field(std::string_view domain_field) {
    if (domain_field == "source.source_id") {
        return "source.id";
    }
    for (const std::string_view field : {
             "client_order_id",
             "source.kind",
             "instrument",
             "side",
             "type",
             "quantity",
             "limit_price",
             "time_in_force",
         }) {
        if (domain_field == field) {
            return std::string(field);
        }
    }
    return std::nullopt;
}

ResponseJson optional_timestamp(const std::optional<Timestamp>& timestamp) {
    return timestamp ? ResponseJson(format_timestamp(*timestamp))
                     : ResponseJson(nullptr);
}

ResponseJson risk_limits_json(const PreTradeRiskLimits& limits) {
    ResponseJson json = ResponseJson::object();
    json["maximum_order_quantity"] = limits.maximum_order_quantity.to_string();
    json["maximum_order_notional"] = limits.maximum_order_notional.to_string();
    json["maximum_position_quantity"] =
        limits.maximum_position_quantity.to_string();
    json["maximum_quote_age_ns"] =
        std::to_string(limits.maximum_quote_age_nanoseconds);
    json["fee_per_fill"] = limits.fee_per_fill.to_string();
    return json;
}

std::string_view account_status(const PaperAccountView& account) {
    return account.is_healthy ? "active" : "halted";
}

// Average entry price is undefined without a position.
ResponseJson average_entry_price_json(const AccountState& state) {
    return state.position.raw() == 0
        ? ResponseJson(nullptr)
        : ResponseJson(state.average_entry_price().to_string());
}

ResponseJson account_json(const PaperAccountView& account) {
    const AccountState& state = account.state;
    ResponseJson json = ResponseJson::object();
    json["trading_mode"] = "paper";
    json["account_id"] = account.account_id;
    json["instrument"] = account.instrument.value();
    json["version"] = std::to_string(account.version);
    json["status"] = account_status(account);
    json["starting_cash"] = state.starting_cash.to_string();
    json["cash"] = state.cash.to_string();
    json["position"] = state.position.to_string();
    json["open_cost_basis"] = state.open_cost_basis.to_string();
    json["average_entry_price"] = average_entry_price_json(state);
    json["gross_realized_pnl"] = state.gross_realized_pnl.to_string();
    json["fees_paid"] = state.fees_paid.to_string();
    json["last_fill_at"] = optional_timestamp(state.last_fill_timestamp);
    json["risk_limits"] = risk_limits_json(account.risk_limits);
    return json;
}

ResponseJson book_level_json(const std::optional<BookLevel>& level) {
    if (!level) {
        return nullptr;
    }
    ResponseJson json = ResponseJson::object();
    json["price"] = level->price.to_string();
    json["quantity"] = level->quantity.to_string();
    return json;
}

ResponseJson quality_flags_json(MarketDataQuality quality) {
    ResponseJson flags = ResponseJson::array();
    for (const auto& [flag, name] : {
             std::pair{MarketDataQualityFlag::Snapshot, "snapshot"},
             std::pair{MarketDataQualityFlag::Replay, "replay"},
             std::pair{
                 MarketDataQualityFlag::QuestionableReceiveTimestamp,
                 "questionable_receive_timestamp",
             },
             std::pair{
                 MarketDataQualityFlag::PotentiallyInvalidBook,
                 "potentially_invalid_book",
             },
             std::pair{
                 MarketDataQualityFlag::SourceSpecific,
                 "source_specific",
             },
         }) {
        if (quality.contains(flag)) {
            flags.push_back(name);
        }
    }
    return flags;
}

ResponseJson market_json(const TopOfBookEvent& market) {
    const MarketEventHeader& header = market.header;
    ResponseJson json = ResponseJson::object();
    json["stream_id"] = header.stream.value();
    json["ingest_sequence"] = std::to_string(header.ingest_sequence);
    json["venue"] = header.venue
        ? ResponseJson(header.venue->value())
        : ResponseJson(nullptr);
    json["event_timestamp"] = format_timestamp(header.event_timestamp);
    json["receive_timestamp"] = format_timestamp(header.receive_timestamp);
    json["quality_flags"] = quality_flags_json(header.quality);
    json["bid"] = book_level_json(market.bid);
    json["ask"] = book_level_json(market.ask);
    return json;
}

ResponseJson source_json(const OrderSource& source) {
    ResponseJson json = ResponseJson::object();
    json["kind"] = name_of(source.kind);
    json["id"] = source.source_id;
    return json;
}

// A fill that accounting booked, with the notional it booked.
struct BookedFill {
    Fill fill;
    Money notional;
};

// A recorded rejection or fault. POST renders it as the response `error`;
// every order resource carries the same object as `rejection`.
struct OrderRejection {
    std::string_view code;
    std::string reason;
    std::optional<std::string> field;
    std::string message;
};

// The part of an order resource that depends on how processing ended, with
// the status a submission of that outcome returns.
struct OrderOutcome {
    std::string status;
    Quantity filled_quantity;
    Quantity unfilled_quantity;
    std::optional<SimulatedCancellationReason> cancellation_reason;
    std::optional<BookedFill> booked_fill;
    std::optional<OrderRejection> rejection;
    int submission_status;
};

// The accounting contract's notional for a fill. The journal stores the
// price and quantity it was computed from, so it is derived identically.
Money booked_notional(const Fill& fill) {
    return calculate_notional(fill.price, fill.quantity);
}

OrderOutcome executed_outcome(
    const SimulatedExecutionReport& execution,
    std::optional<Money> notional
) {
    if (execution.fill.has_value() != notional.has_value()) {
        throw std::logic_error("a simulated fill must be booked exactly once");
    }
    return OrderOutcome{
        .status = name_of(execution.status),
        .filled_quantity = execution.fill ? execution.fill->quantity
                                          : Quantity{},
        .unfilled_quantity = execution.unfilled_quantity,
        .cancellation_reason = execution.cancellation_reason,
        .booked_fill = execution.fill
            ? std::optional<BookedFill>(BookedFill{
                  .fill = *execution.fill,
                  .notional = *notional,
              })
            : std::nullopt,
        .rejection = std::nullopt,
        .submission_status = 201,
    };
}

// Rejected and faulted orders book nothing, so no quantity was filled.
OrderOutcome unexecuted_outcome(
    const OrderIntent& intent,
    std::string status,
    OrderRejection rejection,
    int submission_status
) {
    return OrderOutcome{
        .status = std::move(status),
        .filled_quantity = Quantity{},
        .unfilled_quantity = intent.quantity,
        .cancellation_reason = std::nullopt,
        .booked_fill = std::nullopt,
        .rejection = std::move(rejection),
        .submission_status = submission_status,
    };
}

OrderOutcome fault_outcome(
    const OrderIntent& intent,
    PaperOrderProcessingErrorCode code,
    const std::string& message
) {
    return unexecuted_outcome(
        intent,
        "faulted",
        OrderRejection{
            .code = "processing_fault",
            .reason = name_of(code),
            .field = std::nullopt,
            .message = message,
        },
        500
    );
}

// One overload per way a recorded order can end, whether it comes from a
// submission just processed or from the journal.
OrderOutcome outcome_of(const OrderIntent&, const PaperOrderReceipt& receipt) {
    return executed_outcome(
        receipt.execution,
        receipt.accounting
            ? std::optional<Money>(receipt.accounting->notional)
            : std::nullopt
    );
}

OrderOutcome outcome_of(
    const OrderIntent&,
    const SimulatedExecutionReport& execution
) {
    return executed_outcome(
        execution,
        execution.fill
            ? std::optional<Money>(booked_notional(*execution.fill))
            : std::nullopt
    );
}

OrderOutcome outcome_of(
    const OrderIntent& intent,
    const PreTradeRiskRejection& rejection
) {
    const bool is_stale =
        rejection.code == PreTradeRiskErrorCode::StaleMarketState;
    return unexecuted_outcome(
        intent,
        "rejected",
        OrderRejection{
            .code = is_stale ? "market_data_stale" : "risk_rejected",
            .reason = name_of(rejection.code),
            .field = request_field(rejection.field),
            .message = rejection.message,
        },
        422
    );
}

OrderOutcome outcome_of(
    const OrderIntent& intent,
    const SimulatedExecutionError& rejection
) {
    return unexecuted_outcome(
        intent,
        "rejected",
        OrderRejection{
            .code = "execution_rejected",
            .reason = name_of(rejection.code),
            .field = request_field(rejection.field),
            .message = rejection.message,
        },
        422
    );
}

OrderOutcome outcome_of(
    const OrderIntent& intent,
    const PaperOrderProcessingError& fault
) {
    return fault_outcome(intent, fault.code, fault.message);
}

// The unbooked execution of a fault is deliberately not rendered as a fill.
OrderOutcome outcome_of(
    const OrderIntent& intent,
    const PaperOrderFaultRecord& fault
) {
    return fault_outcome(intent, fault.code, fault.message);
}

template <typename OutcomeVariant>
OrderOutcome order_outcome(
    const OrderIntent& intent,
    const OutcomeVariant& outcome
) {
    return std::visit(
        [&intent](const auto& alternative) {
            return outcome_of(intent, alternative);
        },
        outcome
    );
}

ResponseJson rejection_json(const OrderRejection& rejection) {
    return error_json(
        rejection.code,
        rejection.message,
        rejection.field,
        rejection.reason
    );
}

void write_fill_fields(ResponseJson& json, const Fill& fill, Money notional) {
    json["fill_id"] = fill.fill_id;
    json["side"] = name_of(fill.side);
    json["price"] = fill.price.to_string();
    json["quantity"] = fill.quantity.to_string();
    json["notional"] = notional.to_string();
    json["fee"] = fill.fee.to_string();
    json["filled_at"] = format_timestamp(fill.timestamp);
}

ResponseJson order_json(
    const OrderIntent& intent,
    const TopOfBookEvent& market,
    const OrderOutcome& outcome
) {
    ResponseJson json = ResponseJson::object();
    json["trading_mode"] = "paper";
    json["account_id"] = intent.account_id;
    json["client_order_id"] = intent.client_order_id;
    json["source"] = source_json(intent.source);
    json["instrument"] = intent.instrument.value();
    json["side"] = name_of(intent.side);
    json["type"] = name_of(intent.type);
    json["quantity"] = intent.quantity.to_string();
    json["limit_price"] = intent.limit_price
        ? ResponseJson(intent.limit_price->to_string())
        : ResponseJson(nullptr);
    json["time_in_force"] = name_of(intent.time_in_force);
    json["submitted_at"] = format_timestamp(intent.submitted_at);
    json["status"] = outcome.status;
    json["filled_quantity"] = outcome.filled_quantity.to_string();
    json["unfilled_quantity"] = outcome.unfilled_quantity.to_string();
    json["cancellation_reason"] = outcome.cancellation_reason
        ? ResponseJson(name_of(*outcome.cancellation_reason))
        : ResponseJson(nullptr);
    ResponseJson fill = nullptr;
    if (outcome.booked_fill) {
        fill = ResponseJson::object();
        write_fill_fields(
            fill,
            outcome.booked_fill->fill,
            outcome.booked_fill->notional
        );
    }
    json["fill"] = std::move(fill);
    json["rejection"] = outcome.rejection
        ? rejection_json(*outcome.rejection)
        : ResponseJson(nullptr);
    json["market"] = market_json(market);
    return json;
}

ResponseJson order_record_json(const PaperOrderRecord& record) {
    return order_json(
        record.intent,
        record.market,
        order_outcome(record.intent, record.outcome)
    );
}

ResponseJson fill_record_json(const PaperFillRecord& record) {
    ResponseJson json = ResponseJson::object();
    json["trading_mode"] = "paper";
    json["account_id"] = record.account_id;
    json["client_order_id"] = record.client_order_id;
    json["source"] = source_json(record.source);
    json["instrument"] = record.fill.instrument.value();
    write_fill_fields(json, record.fill, booked_notional(record.fill));
    return json;
}

// MarketState could not quote the instrument, or valuation rejected the quote.
std::optional<std::string> market_unavailable_reason(
    const PaperTradingError& failure
) {
    if (failure.market_state) {
        return std::string(market_state_reason(*failure.market_state));
    }
    if (failure.valuation) {
        return std::string(valuation_reason(*failure.valuation));
    }
    return std::nullopt;
}

// Mark-to-market at the best bid; see docs/paper-trading-api.md.
ResponseJson portfolio_json(const PaperPortfolioView& portfolio) {
    const PaperAccountView& account = portfolio.account;
    const PortfolioValuation& valuation = portfolio.valuation;
    const AccountSnapshot& snapshot = valuation.snapshot;
    const AccountState& state = snapshot.state;

    ResponseJson json = ResponseJson::object();
    json["trading_mode"] = "paper";
    json["account_id"] = account.account_id;
    json["instrument"] = account.instrument.value();
    json["account_version"] = std::to_string(account.version);
    json["status"] = account_status(account);
    json["valued_at"] = format_timestamp(valuation.valued_at);
    json["mark_policy"] = "best_bid";
    json["mark_price"] = snapshot.mark_price.to_string();
    json["market"] = market_json(valuation.market);
    json["starting_cash"] = state.starting_cash.to_string();
    json["cash"] = state.cash.to_string();
    json["position"] = state.position.to_string();
    json["open_cost_basis"] = state.open_cost_basis.to_string();
    json["average_entry_price"] = average_entry_price_json(state);
    json["position_market_value"] = snapshot.market_value.to_string();
    json["unrealized_pnl"] = snapshot.gross_unrealized_pnl.to_string();
    json["gross_realized_pnl"] = state.gross_realized_pnl.to_string();
    json["fees_paid"] = state.fees_paid.to_string();
    json["total_pnl"] = snapshot.net_total_pnl.to_string();
    json["equity"] = snapshot.equity.to_string();
    json["return_percent"] = valuation.return_percent.to_string();
    return json;
}

HttpResponse render_service_error(const PaperTradingError& failure) {
    switch (failure.code) {
        case PaperTradingErrorCode::InvalidIdentifier:
            return error_response(
                400,
                "malformed_request",
                failure.message,
                nonempty(failure.field),
                "invalid_identifier"
            );
        case PaperTradingErrorCode::UnsupportedInstrument:
            return error_response(
                422,
                "unsupported_instrument",
                failure.message,
                nonempty(failure.field)
            );
        case PaperTradingErrorCode::InvalidStartingCash:
            return error_response(
                422,
                "invalid_starting_cash",
                failure.message,
                nonempty(failure.field)
            );
        case PaperTradingErrorCode::InvalidPageLimit:
            return error_response(
                400,
                "malformed_request",
                failure.message,
                nonempty(failure.field),
                "invalid_limit"
            );
        case PaperTradingErrorCode::InvalidCursor:
            return error_response(
                400,
                "malformed_request",
                failure.message,
                nonempty(failure.field),
                "invalid_cursor"
            );
        case PaperTradingErrorCode::AccountAlreadyExists:
            return error_response(
                409,
                "account_already_exists",
                failure.message,
                nonempty(failure.field)
            );
        case PaperTradingErrorCode::AccountNotFound:
            return error_response(
                404,
                "account_not_found",
                failure.message,
                nonempty(failure.field)
            );
        case PaperTradingErrorCode::OrderNotFound:
            return error_response(
                404,
                "order_not_found",
                failure.message,
                nonempty(failure.field)
            );
        case PaperTradingErrorCode::MarketDataUnavailable:
            return error_response(
                503,
                "market_data_unavailable",
                failure.message,
                std::nullopt,
                market_unavailable_reason(failure)
            );
        case PaperTradingErrorCode::DuplicateClientOrderId:
            return error_response(
                409,
                "duplicate_client_order_id",
                "client_order_id was already used for this account",
                nonempty(failure.field)
            );
        case PaperTradingErrorCode::AccountVersionConflict:
            return error_response(
                409,
                "account_version_conflict",
                "the account changed while the order was processed; nothing "
                "was recorded, so the same client_order_id may be retried"
            );
        case PaperTradingErrorCode::ValuationFailed:
            return error_response(
                500,
                "valuation_failed",
                failure.message,
                std::nullopt,
                failure.valuation
                    ? std::optional<std::string>(
                          valuation_reason(*failure.valuation)
                      )
                    : std::nullopt
            );
        case PaperTradingErrorCode::PersistenceUnavailable:
            // The underlying libpq message stays in server logs.
            return error_response(
                503,
                "persistence_unavailable",
                "paper-trading persistence is unavailable; if this request "
                "writes data, it may or may not have completed, so retry the "
                "same request with the same identifiers"
            );
    }
    throw std::logic_error("unknown paper-trading error");
}

void append_padded(std::string& text, std::int64_t value, int width) {
    const std::string digits = std::to_string(value);
    text.append(
        static_cast<std::size_t>(
            width > static_cast<int>(digits.size())
                ? width - static_cast<int>(digits.size())
                : 0
        ),
        '0'
    );
    text += digits;
}

}  // namespace

std::variant<CreatePaperAccountRequest, MalformedRequest>
parse_create_account_request(std::string_view body) {
    auto document = parse_json_object(body);
    if (const auto* invalid = std::get_if<MalformedRequest>(&document)) {
        return *invalid;
    }

    std::optional<MalformedRequest> error;
    FieldReader fields(
        std::get<RequestJson>(document),
        "",
        {"account_id", "instrument", "starting_cash"},
        error
    );
    auto account_id = fields.string("account_id");
    auto instrument = fields.string("instrument");
    const auto starting_cash = fields.decimal<Money>("starting_cash");
    if (error) {
        return *error;
    }
    return CreatePaperAccountRequest{
        .account_id = std::move(*account_id),
        .instrument = std::move(*instrument),
        .starting_cash = *starting_cash,
    };
}

std::variant<PaperOrderRequest, MalformedRequest> parse_order_request(
    std::string account_id,
    std::string_view body
) {
    auto document = parse_json_object(body);
    if (const auto* invalid = std::get_if<MalformedRequest>(&document)) {
        return *invalid;
    }

    std::optional<MalformedRequest> error;
    FieldReader fields(
        std::get<RequestJson>(document),
        "",
        {
            "client_order_id",
            "source",
            "instrument",
            "side",
            "type",
            "quantity",
            "limit_price",
            "time_in_force",
        },
        error
    );
    auto client_order_id = fields.string("client_order_id");
    std::optional<OrderSourceKind> source_kind;
    std::optional<std::string> source_id;
    if (auto source = fields.object("source", {"kind", "id"})) {
        source_kind = source->enumeration<OrderSourceKind>("kind");
        source_id = source->string("id");
    }
    auto instrument = fields.string("instrument");
    const auto side = fields.enumeration<Side>("side");
    const auto type = fields.enumeration<OrderType>("type");
    const auto quantity = fields.decimal<Quantity>("quantity");
    std::optional<Price> limit_price;
    if (fields.contains("limit_price")) {
        limit_price = fields.decimal<Price>("limit_price");
    }
    const auto time_in_force = fields.enumeration<TimeInForce>(
        "time_in_force"
    );
    if (error) {
        return *error;
    }
    return PaperOrderRequest{
        .account_id = std::move(account_id),
        .client_order_id = std::move(*client_order_id),
        .source = OrderSource{
            .kind = *source_kind,
            .source_id = std::move(*source_id),
        },
        .instrument = std::move(*instrument),
        .side = *side,
        .type = *type,
        .quantity = *quantity,
        .limit_price = limit_price,
        .time_in_force = *time_in_force,
    };
}

std::variant<HistoryPageRequest, MalformedRequest> parse_history_query(
    std::string_view query
) {
    auto parsed = parse_query(query, {"limit", "cursor"});
    if (auto* invalid = std::get_if<MalformedRequest>(&parsed)) {
        return std::move(*invalid);
    }
    HistoryPageRequest request;
    for (QueryParameter& parameter :
         std::get<std::vector<QueryParameter>>(parsed)) {
        if (parameter.name == "cursor") {
            request.cursor = std::move(parameter.value);
            continue;
        }
        request.limit = parse_limit(parameter.value);
        if (!request.limit) {
            return malformed(
                MalformedRequestReason::InvalidLimit,
                "limit",
                "limit must be an integer from 1 to " +
                    std::to_string(kMaximumHistoryPageLimit)
            );
        }
    }
    return request;
}

std::optional<MalformedRequest> reject_query_parameters(
    std::string_view query
) {
    auto parsed = parse_query(query, {});
    if (auto* invalid = std::get_if<MalformedRequest>(&parsed)) {
        return std::move(*invalid);
    }
    return std::nullopt;
}

std::string format_timestamp(Timestamp timestamp) {
    constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
    constexpr std::int64_t kNanosecondsPerDay =
        86'400 * kNanosecondsPerSecond;

    // Floor division keeps pre-epoch instants on the correct calendar day
    // without converting the whole range into a wider chrono duration.
    std::int64_t days = timestamp.nanoseconds() / kNanosecondsPerDay;
    std::int64_t nanoseconds_of_day =
        timestamp.nanoseconds() % kNanosecondsPerDay;
    if (nanoseconds_of_day < 0) {
        nanoseconds_of_day += kNanosecondsPerDay;
        --days;
    }
    const std::chrono::year_month_day date{std::chrono::sys_days{
        std::chrono::days{static_cast<std::chrono::days::rep>(days)},
    }};
    const std::int64_t seconds_of_day =
        nanoseconds_of_day / kNanosecondsPerSecond;

    std::string text;
    append_padded(text, static_cast<int>(date.year()), 4);
    text += '-';
    append_padded(text, static_cast<unsigned>(date.month()), 2);
    text += '-';
    append_padded(text, static_cast<unsigned>(date.day()), 2);
    text += 'T';
    append_padded(text, seconds_of_day / 3'600, 2);
    text += ':';
    append_padded(text, seconds_of_day / 60 % 60, 2);
    text += ':';
    append_padded(text, seconds_of_day % 60, 2);
    text += '.';
    append_padded(text, nanoseconds_of_day % kNanosecondsPerSecond, 9);
    text += 'Z';
    return text;
}

HttpResponse render_health(const PaperTradingHealth& health) {
    const MarketStateStatus& market = health.market;
    ResponseJson market_data = ResponseJson::object();
    market_data["stream_id"] = market.stream.value();
    market_data["state"] = market.interruption ? "interrupted" : "accepting";
    market_data["last_ingest_sequence"] = market.last_ingest_sequence
        ? ResponseJson(std::to_string(*market.last_ingest_sequence))
        : ResponseJson(nullptr);
    market_data["market_time"] = optional_timestamp(market.market_time);
    market_data["interruption"] = market.interruption
        ? ResponseJson(market.interruption->message)
        : ResponseJson(nullptr);

    ResponseJson instruments = ResponseJson::array();
    for (const InstrumentId& instrument : health.tradeable_instruments) {
        instruments.push_back(instrument.value());
    }

    ResponseJson body = ResponseJson::object();
    body["status"] = health.persistence_available ? "ok" : "unavailable";
    body["trading_mode"] = "paper";
    body["persistence"] =
        health.persistence_available ? "available" : "unavailable";
    body["tradeable_instruments"] = std::move(instruments);
    body["market_data"] = std::move(market_data);
    return json_response(health.persistence_available ? 200 : 503, body);
}

HttpResponse render_account(
    const PaperAccountResult& result,
    int success_status
) {
    if (const auto* failure = std::get_if<PaperTradingError>(&result)) {
        return render_service_error(*failure);
    }
    ResponseJson body = ResponseJson::object();
    body["account"] = account_json(std::get<PaperAccountView>(result));
    return json_response(success_status, body);
}

HttpResponse render_order_submission(
    const PaperOrderSubmissionResult& result
) {
    if (const auto* failure = std::get_if<PaperTradingError>(&result)) {
        return render_service_error(*failure);
    }
    const auto& recorded = std::get<RecordedPaperOrder>(result);
    const OrderOutcome outcome =
        order_outcome(recorded.intent, recorded.result);
    ResponseJson body = ResponseJson::object();
    if (outcome.rejection) {
        body["error"] = rejection_json(*outcome.rejection);
    }
    body["order"] = order_json(recorded.intent, recorded.market, outcome);
    body["account"] = account_json(recorded.account);
    return json_response(outcome.submission_status, body);
}

HttpResponse render_order_history(const PaperOrderHistoryResult& result) {
    if (const auto* failure = std::get_if<PaperTradingError>(&result)) {
        return render_service_error(*failure);
    }
    const auto& page = std::get<PaperOrderHistoryPage>(result);
    ResponseJson orders = ResponseJson::array();
    for (const PaperOrderRecord& record : page.orders) {
        orders.push_back(order_record_json(record));
    }
    ResponseJson body = ResponseJson::object();
    body["orders"] = std::move(orders);
    body["next_cursor"] = nullable(page.next_cursor);
    return json_response(200, body);
}

HttpResponse render_order_record(const PaperOrderRecordResult& result) {
    if (const auto* failure = std::get_if<PaperTradingError>(&result)) {
        return render_service_error(*failure);
    }
    ResponseJson body = ResponseJson::object();
    body["order"] = order_record_json(std::get<PaperOrderRecord>(result));
    return json_response(200, body);
}

HttpResponse render_fill_history(const PaperFillHistoryResult& result) {
    if (const auto* failure = std::get_if<PaperTradingError>(&result)) {
        return render_service_error(*failure);
    }
    const auto& page = std::get<PaperFillHistoryPage>(result);
    ResponseJson fills = ResponseJson::array();
    for (const PaperFillRecord& record : page.fills) {
        fills.push_back(fill_record_json(record));
    }
    ResponseJson body = ResponseJson::object();
    body["fills"] = std::move(fills);
    body["next_cursor"] = nullable(page.next_cursor);
    return json_response(200, body);
}

HttpResponse render_portfolio(const PaperPortfolioResult& result) {
    if (const auto* failure = std::get_if<PaperTradingError>(&result)) {
        return render_service_error(*failure);
    }
    ResponseJson body = ResponseJson::object();
    body["portfolio"] = portfolio_json(std::get<PaperPortfolioView>(result));
    return json_response(200, body);
}

HttpResponse render_malformed_request(const MalformedRequest& malformed) {
    return error_response(
        400,
        "malformed_request",
        malformed.message,
        nonempty(malformed.field),
        std::string(reason_name(malformed.reason))
    );
}

HttpResponse render_error(
    int status,
    std::string_view code,
    std::string_view message
) {
    return error_response(status, code, message);
}

}  // namespace chronos::paper_trading
