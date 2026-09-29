#include "chronos/alpaca/alpaca_protocol.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::alpaca_adapter {
namespace {

using Json = nlohmann::json;

// A JSON number exactly as written in the frame.
struct JsonNumber {
    std::string lexeme;
};

// A nested value Chronos never reads; only its presence is kept.
struct OtherValue {};

using FieldValue = std::variant<
    std::nullptr_t,
    bool,
    std::string,
    JsonNumber,
    std::vector<std::string>,
    OtherValue
>;

struct FlatMessage {
    std::vector<std::pair<std::string, FieldValue>> fields;

    [[nodiscard]] const FieldValue* find(std::string_view name) const {
        for (const auto& [key, value] : fields) {
            if (key == name) {
                return &value;
            }
        }
        return nullptr;
    }
};

// Reads a frame as an array of flat objects. Numbers keep their lexeme: the
// double that nlohmann computes for a decimal token is discarded unread.
class FrameReader final : public nlohmann::json_sax<Json> {
public:
    bool null() override { return scalar(nullptr); }
    bool boolean(bool value) override { return scalar(value); }
    bool number_integer(number_integer_t value) override {
        return scalar(JsonNumber{std::to_string(value)});
    }
    bool number_unsigned(number_unsigned_t value) override {
        return scalar(JsonNumber{std::to_string(value)});
    }
    bool number_float(number_float_t, const string_t& lexeme) override {
        return scalar(JsonNumber{lexeme});
    }
    bool string(string_t& value) override { return scalar(std::move(value)); }
    bool binary(binary_t&) override {
        return fail("the frame contains a binary value");
    }

    bool start_object(std::size_t) override {
        if (skip_depth_ > 0) {
            ++skip_depth_;
            return true;
        }
        switch (level_) {
            case Level::Top:
                return fail("a frame must be a JSON array of messages");
            case Level::Frame:
                messages_.emplace_back();
                level_ = Level::Message;
                return true;
            case Level::Message:
                skip_depth_ = 1;
                return true;
            case Level::FieldList:
                list_is_other_ = true;
                skip_depth_ = 1;
                return true;
        }
        return false;
    }

    bool key(string_t& name) override {
        if (skip_depth_ == 0) {
            key_ = std::move(name);
        }
        return true;
    }

    bool end_object() override {
        if (skip_depth_ > 0) {
            return end_skipped();
        }
        level_ = Level::Frame;
        return true;
    }

    bool start_array(std::size_t) override {
        if (skip_depth_ > 0) {
            ++skip_depth_;
            return true;
        }
        switch (level_) {
            case Level::Top:
                level_ = Level::Frame;
                saw_frame_ = true;
                return true;
            case Level::Frame:
                return fail(message_label(messages_.size()) + " is not a JSON object");
            case Level::Message:
                list_.clear();
                list_is_other_ = false;
                level_ = Level::FieldList;
                return true;
            case Level::FieldList:
                list_is_other_ = true;
                skip_depth_ = 1;
                return true;
        }
        return false;
    }

    bool end_array() override {
        if (skip_depth_ > 0) {
            return end_skipped();
        }
        if (level_ == Level::FieldList) {
            level_ = Level::Message;
            if (list_is_other_) {
                return set_field(OtherValue{});
            }
            return set_field(std::exchange(list_, {}));
        }
        level_ = Level::Top;
        return true;
    }

    bool parse_error(
        std::size_t position,
        const std::string&,
        const nlohmann::detail::exception&
    ) override {
        // The offending token is frame content, so it is not repeated.
        if (error_.empty()) {
            error_ = "the frame is not valid JSON (at byte " +
                std::to_string(position) + ")";
        }
        return false;
    }

    [[nodiscard]] bool saw_frame() const noexcept { return saw_frame_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::vector<FlatMessage>& messages() noexcept {
        return messages_;
    }

    static std::string message_label(std::size_t index) {
        return "message " + std::to_string(index + 1);
    }

private:
    enum class Level {
        Top,
        Frame,
        Message,
        FieldList,
    };

    bool scalar(FieldValue value) {
        if (skip_depth_ > 0) {
            return true;
        }
        switch (level_) {
            case Level::Top:
                return fail("a frame must be a JSON array of messages");
            case Level::Frame:
                return fail(message_label(messages_.size()) + " is not a JSON object");
            case Level::Message:
                return set_field(std::move(value));
            case Level::FieldList:
                if (auto* text = std::get_if<std::string>(&value)) {
                    list_.push_back(std::move(*text));
                } else {
                    list_is_other_ = true;
                }
                return true;
        }
        return false;
    }

    bool end_skipped() {
        --skip_depth_;
        if (skip_depth_ == 0 && level_ == Level::Message) {
            return set_field(OtherValue{});
        }
        return true;
    }

    bool set_field(FieldValue value) {
        FlatMessage& message = messages_.back();
        if (message.find(key_) != nullptr) {
            return fail(
                message_label(messages_.size() - 1) + " repeats the field \"" +
                printable_excerpt(key_) + "\""
            );
        }
        message.fields.emplace_back(std::move(key_), std::move(value));
        key_.clear();
        return true;
    }

    bool fail(std::string message) {
        error_ = std::move(message);
        return false;
    }

    Level level_{Level::Top};
    std::size_t skip_depth_{0};
    bool saw_frame_{false};
    std::string key_;
    std::vector<std::string> list_;
    bool list_is_other_{false};
    std::vector<FlatMessage> messages_;
    std::string error_;
};

// Reads typed fields of one message, naming the message in every error.
class FieldReader final {
public:
    FieldReader(const FlatMessage& message, std::string label)
        : message_(message),
          label_(std::move(label)) {}

    [[nodiscard]] std::optional<std::string> string(std::string_view name) {
        const FieldValue* value = present(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (const auto* text = std::get_if<std::string>(value)) {
            return *text;
        }
        return wrong_type(name, "a string");
    }

    [[nodiscard]] std::optional<std::string> nonempty_string(
        std::string_view name
    ) {
        auto text = string(name);
        if (text && text->empty()) {
            fail("field \"" + std::string(name) + "\" must not be empty");
            return std::nullopt;
        }
        return text;
    }

    [[nodiscard]] std::optional<std::string> number(std::string_view name) {
        const FieldValue* value = present(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (const auto* number = std::get_if<JsonNumber>(value)) {
            return number->lexeme;
        }
        return wrong_type(name, "a number");
    }

    // Absent or null is nullopt without an error.
    [[nodiscard]] std::optional<std::string> optional_string(
        std::string_view name
    ) {
        const FieldValue* value = message_.find(name);
        if (value == nullptr || std::holds_alternative<std::nullptr_t>(*value)) {
            return std::nullopt;
        }
        if (const auto* text = std::get_if<std::string>(value)) {
            return *text;
        }
        return wrong_type(name, "a string");
    }

    [[nodiscard]] std::vector<std::string> string_list(
        std::string_view name,
        bool required
    ) {
        const FieldValue* value = message_.find(name);
        if (value == nullptr || std::holds_alternative<std::nullptr_t>(*value)) {
            if (required) {
                fail("field \"" + std::string(name) + "\" is missing");
            }
            return {};
        }
        if (const auto* list = std::get_if<std::vector<std::string>>(value)) {
            return *list;
        }
        static_cast<void>(wrong_type(name, "an array of strings"));
        return {};
    }

    [[nodiscard]] const std::optional<std::string>& error() const noexcept {
        return error_;
    }

    void fail(std::string detail) {
        if (!error_) {
            error_ = label_ + ": " + std::move(detail);
        }
    }

private:
    const FieldValue* present(std::string_view name) {
        const FieldValue* value = message_.find(name);
        if (value == nullptr || std::holds_alternative<std::nullptr_t>(*value)) {
            fail("field \"" + std::string(name) + "\" is missing");
            return nullptr;
        }
        return value;
    }

    std::nullopt_t wrong_type(std::string_view name, std::string_view type) {
        fail("field \"" + std::string(name) + "\" must be " + std::string(type));
        return std::nullopt;
    }

    const FlatMessage& message_;
    std::string label_;
    std::optional<std::string> error_;
};

bool is_ignored_type(std::string_view type) {
    // c: trade correction, x: trade cancel/error, s: trading status,
    // l: LULD band, b/d/u: bars, i: order imbalance.
    for (const std::string_view ignored : {"c", "x", "s", "l", "b", "d", "u", "i"}) {
        if (type == ignored) {
            return true;
        }
    }
    return false;
}

std::optional<std::int64_t> integer_code(std::string_view lexeme) {
    std::int64_t value = 0;
    const auto result = std::from_chars(
        lexeme.data(),
        lexeme.data() + lexeme.size(),
        value
    );
    if (result.ec != std::errc{} || result.ptr != lexeme.data() + lexeme.size()) {
        return std::nullopt;
    }
    return value;
}

std::variant<AlpacaMessage, std::string> decode_message(
    const FlatMessage& message,
    std::size_t index
) {
    const std::string label = FrameReader::message_label(index);
    FieldReader type_reader(message, label);
    const auto type = type_reader.nonempty_string("T");
    if (!type) {
        return *type_reader.error();
    }

    FieldReader fields(message, label + " (type \"" + printable_excerpt(*type) + "\")");
    if (*type == "success") {
        auto text = fields.string("msg");
        if (!text) {
            return *fields.error();
        }
        return AlpacaMessage(AlpacaSuccess{.message = std::move(*text)});
    }
    if (*type == "error") {
        auto code = fields.number("code");
        auto text = fields.string("msg");
        if (fields.error()) {
            return *fields.error();
        }
        const auto parsed = integer_code(*code);
        if (!parsed) {
            fields.fail("field \"code\" must be an integer");
            return *fields.error();
        }
        return AlpacaMessage(AlpacaErrorMessage{
            .code = *parsed,
            .message = std::move(*text),
        });
    }
    if (*type == "subscription") {
        auto trades = fields.string_list("trades", true);
        auto quotes = fields.string_list("quotes", true);
        if (fields.error()) {
            return *fields.error();
        }
        return AlpacaMessage(AlpacaSubscriptionMessage{
            .trades = std::move(trades),
            .quotes = std::move(quotes),
        });
    }
    if (*type == "q") {
        AlpacaQuote quote{
            .symbol = fields.nonempty_string("S").value_or(""),
            .bid_exchange = fields.optional_string("bx"),
            .bid_price = fields.number("bp").value_or(""),
            .bid_size = fields.number("bs").value_or(""),
            .ask_exchange = fields.optional_string("ax"),
            .ask_price = fields.number("ap").value_or(""),
            .ask_size = fields.number("as").value_or(""),
            .timestamp = fields.string("t").value_or(""),
            .conditions = fields.string_list("c", false),
            .tape = fields.optional_string("z"),
        };
        if (fields.error()) {
            return *fields.error();
        }
        return AlpacaMessage(std::move(quote));
    }
    if (*type == "t") {
        AlpacaTrade trade{
            .symbol = fields.nonempty_string("S").value_or(""),
            .trade_id = fields.number("i").value_or(""),
            .exchange = fields.string("x").value_or(""),
            .price = fields.number("p").value_or(""),
            .size = fields.number("s").value_or(""),
            .timestamp = fields.string("t").value_or(""),
            .conditions = fields.string_list("c", false),
            .tape = fields.optional_string("z"),
        };
        if (fields.error()) {
            return *fields.error();
        }
        return AlpacaMessage(std::move(trade));
    }
    if (is_ignored_type(*type)) {
        return AlpacaMessage(AlpacaIgnoredMessage{.type = *type});
    }
    return label + " has the unknown type \"" + printable_excerpt(*type) + "\"";
}

}  // namespace

AlpacaDecodeResult decode_alpaca_frame(std::string_view frame) {
    FrameReader reader;
    const bool parsed = Json::sax_parse(frame, &reader);
    if (!parsed || !reader.saw_frame()) {
        return AlpacaDecodeError{
            .message = reader.error().empty()
                ? "a frame must be a JSON array of messages"
                : reader.error(),
        };
    }

    std::vector<AlpacaMessage> messages;
    messages.reserve(reader.messages().size());
    for (std::size_t index = 0; index < reader.messages().size(); ++index) {
        auto decoded = decode_message(reader.messages()[index], index);
        if (auto* error = std::get_if<std::string>(&decoded)) {
            return AlpacaDecodeError{.message = std::move(*error)};
        }
        messages.push_back(std::get<AlpacaMessage>(std::move(decoded)));
    }
    return messages;
}

bool is_control_message(const AlpacaMessage& message) noexcept {
    return std::holds_alternative<AlpacaSuccess>(message) ||
        std::holds_alternative<AlpacaErrorMessage>(message) ||
        std::holds_alternative<AlpacaSubscriptionMessage>(message);
}

std::string printable_excerpt(std::string_view text) {
    constexpr std::size_t kMaximumLength = 120;
    std::string excerpt;
    for (const char character : text.substr(0, kMaximumLength)) {
        const auto code = static_cast<unsigned char>(character);
        excerpt.push_back(code >= 0x20 && code < 0x7f ? character : '?');
    }
    if (text.size() > kMaximumLength) {
        excerpt += "...";
    }
    return excerpt;
}

}  // namespace chronos::alpaca_adapter
