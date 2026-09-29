#include "chronos/alpaca/alpaca_credentials.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace chronos::alpaca_adapter {
namespace {

std::variant<std::string, AlpacaCredentialsError> read_variable(
    const AlpacaCredentials::EnvironmentLookup& lookup,
    std::string_view name
) {
    std::optional<std::string> value = lookup(name);
    if (!value) {
        return AlpacaCredentialsError{
            .message = std::string(name) + " is not set",
        };
    }
    if (value->empty()) {
        return AlpacaCredentialsError{
            .message = std::string(name) + " is empty",
        };
    }
    const bool printable = std::all_of(value->begin(), value->end(), [](char character) {
        const auto code = static_cast<unsigned char>(character);
        return code > 0x20 && code < 0x7f;
    });
    if (!printable) {
        wipe(*value);
        return AlpacaCredentialsError{
            .message = std::string(name) +
                " contains whitespace or control characters; set it to the "
                "credential alone",
        };
    }
    return std::move(*value);
}

void replace_all(std::string& text, std::string_view secret) {
    if (secret.empty()) {
        return;
    }
    constexpr std::string_view kReplacement = "[REDACTED]";
    std::size_t position = 0;
    while ((position = text.find(secret, position)) != std::string::npos) {
        text.replace(position, secret.size(), kReplacement);
        position += kReplacement.size();
    }
}

}  // namespace

void wipe(std::string& secret) noexcept {
    volatile char* characters = secret.data();
    for (std::size_t index = 0; index < secret.size(); ++index) {
        characters[index] = '\0';
    }
    secret.clear();
}

std::variant<AlpacaCredentials, AlpacaCredentialsError> AlpacaCredentials::from_environment(
    const EnvironmentLookup& lookup
) {
    auto key_id = read_variable(lookup, kAlpacaKeyIdVariable);
    if (auto* error = std::get_if<AlpacaCredentialsError>(&key_id)) {
        return std::move(*error);
    }
    auto secret_key = read_variable(lookup, kAlpacaSecretKeyVariable);
    if (auto* error = std::get_if<AlpacaCredentialsError>(&secret_key)) {
        wipe(std::get<std::string>(key_id));
        return std::move(*error);
    }
    return AlpacaCredentials(
        std::get<std::string>(std::move(key_id)),
        std::get<std::string>(std::move(secret_key))
    );
}

AlpacaCredentials::EnvironmentLookup AlpacaCredentials::process_environment() {
    return [](std::string_view name) -> std::optional<std::string> {
        const char* value = std::getenv(std::string(name).c_str());
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string(value);
    };
}

AlpacaCredentials::AlpacaCredentials(std::string key_id, std::string secret_key)
    : key_id_(std::move(key_id)),
      secret_key_(std::move(secret_key)) {}

AlpacaCredentials::AlpacaCredentials(AlpacaCredentials&& other) noexcept
    : key_id_(std::move(other.key_id_)),
      secret_key_(std::move(other.secret_key_)) {
    wipe(other.key_id_);
    wipe(other.secret_key_);
}

AlpacaCredentials& AlpacaCredentials::operator=(AlpacaCredentials&& other) noexcept {
    if (this != &other) {
        wipe(key_id_);
        wipe(secret_key_);
        key_id_ = std::move(other.key_id_);
        secret_key_ = std::move(other.secret_key_);
        wipe(other.key_id_);
        wipe(other.secret_key_);
    }
    return *this;
}

AlpacaCredentials::~AlpacaCredentials() {
    wipe(key_id_);
    wipe(secret_key_);
}

std::string AlpacaCredentials::authentication_message() const {
    nlohmann::ordered_json message = nlohmann::ordered_json::object();
    message["action"] = "auth";
    message["key"] = key_id_;
    message["secret"] = secret_key_;
    return message.dump();
}

std::string AlpacaCredentials::redact(std::string text) const {
    replace_all(text, secret_key_);
    replace_all(text, key_id_);
    return text;
}

}  // namespace chronos::alpaca_adapter
