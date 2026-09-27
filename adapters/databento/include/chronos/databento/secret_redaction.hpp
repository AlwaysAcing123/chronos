#pragma once

#include <string>
#include <string_view>

namespace chronos::databento_adapter {

[[nodiscard]] std::string redact_secret(
    std::string message,
    std::string_view secret
);

}  // namespace chronos::databento_adapter
