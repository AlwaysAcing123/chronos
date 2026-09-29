#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace chronos::alpaca_adapter {

inline constexpr std::string_view kAlpacaKeyIdVariable = "APCA_API_KEY_ID";
inline constexpr std::string_view kAlpacaSecretKeyVariable = "APCA_API_SECRET_KEY";

struct AlpacaCredentialsError {
    // Names the variable, never its value.
    std::string message;
};

// Alpaca market-data credentials, read only from the environment.
//
// The values never appear in a diagnostic: the type has no accessor or
// stream operator, only the authentication message and redact(). They are
// never accepted on a command line and never sent to a browser. The
// strings are overwritten when the object is destroyed, as a best effort
// against lingering copies in freed memory.
class AlpacaCredentials final {
public:
    // Returns the value of an environment variable, or nullopt when unset.
    using EnvironmentLookup =
        std::function<std::optional<std::string>(std::string_view name)>;

    // Reads APCA_API_KEY_ID and APCA_API_SECRET_KEY. Each must be set,
    // non-empty, and free of whitespace and control characters.
    [[nodiscard]] static std::variant<AlpacaCredentials, AlpacaCredentialsError>
    from_environment(const EnvironmentLookup& lookup);

    // The process environment, through std::getenv.
    [[nodiscard]] static EnvironmentLookup process_environment();

    AlpacaCredentials(AlpacaCredentials&& other) noexcept;
    AlpacaCredentials& operator=(AlpacaCredentials&& other) noexcept;
    AlpacaCredentials(const AlpacaCredentials&) = delete;
    AlpacaCredentials& operator=(const AlpacaCredentials&) = delete;
    ~AlpacaCredentials();

    // {"action":"auth","key":...,"secret":...}. The caller sends it and
    // must never log, store, or echo it.
    [[nodiscard]] std::string authentication_message() const;

    // Replaces every occurrence of either credential with "[REDACTED]".
    [[nodiscard]] std::string redact(std::string text) const;

private:
    AlpacaCredentials(std::string key_id, std::string secret_key);

    std::string key_id_;
    std::string secret_key_;
};

// Overwrites a string's characters before it is released.
void wipe(std::string& secret) noexcept;

}  // namespace chronos::alpaca_adapter
