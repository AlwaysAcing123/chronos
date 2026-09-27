#include "chronos/databento/secret_redaction.hpp"
#include "test_support.hpp"

#include <string>

namespace {

namespace adapter = chronos::databento_adapter;

void test_redacts_every_occurrence() {
    const std::string secret = "private-key";
    const std::string message =
        "authentication rejected private-key; username=private-key";

    const std::string redacted = adapter::redact_secret(message, secret);
    REQUIRE(
        redacted ==
        "authentication rejected [REDACTED]; username=[REDACTED]"
    );
    REQUIRE(redacted.find(secret) == std::string::npos);
}

void test_preserves_message_without_secret() {
    const std::string message = "connection timed out";
    REQUIRE(adapter::redact_secret(message, "private-key") == message);
}

void test_empty_secret_never_rewrites_message() {
    const std::string message = "provider returned an error";
    REQUIRE(adapter::redact_secret(message, "") == message);
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"all secret occurrences", test_redacts_every_occurrence},
        {"message without secret", test_preserves_message_without_secret},
        {"empty secret", test_empty_secret_never_rewrites_message},
    });
}
