#include "chronos/databento/secret_redaction.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace chronos::databento_adapter {

std::string redact_secret(
    std::string message,
    std::string_view secret
) {
    if (secret.empty()) {
        return message;
    }

    constexpr std::string_view kReplacement = "[REDACTED]";
    std::size_t position = 0;
    while ((position = message.find(secret, position)) != std::string::npos) {
        message.replace(position, secret.size(), kReplacement);
        position += kReplacement.size();
    }
    return message;
}

}  // namespace chronos::databento_adapter
