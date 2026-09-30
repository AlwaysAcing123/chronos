#include "dataset_cli_support.hpp"
#include "test_support.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

namespace cli = chronos::dataset_cli;
namespace json = chronos::canonical_json;

// Names and paths are bytes on some file systems; output must still be one
// JSON document, so bytes JSON cannot carry are escaped, not fatal.
void test_output_text_escapes_what_json_cannot_carry() {
    REQUIRE(cli::printable("AAPL") == "AAPL");
    REQUIRE(cli::printable("caf\xC3\xA9") == "caf\xC3\xA9");
    REQUIRE(cli::printable("bad\xFFname") == "bad\\xffname");
    REQUIRE(cli::printable("\xC3") == "\\xc3");
    REQUIRE(cli::printable("\xE2\x82") == "\\xe2\\x82");
    REQUIRE(cli::printable("\xF0\x9F\x98\x80!") == "\xF0\x9F\x98\x80!");
    const auto error = chronos::datasets::dataset_error(
        chronos::datasets::DatasetErrorCode::UnexpectedEntry,
        "stray\xFE",
        "is not part of a normalized dataset: \xC0\xAF"
    );
    const std::string document = json::serialize(cli::error_json(error));
    REQUIRE(document.find("stray\\\\xfe") != std::string::npos);
    REQUIRE(document.find("\\\\xc0\\\\xaf") != std::string::npos);
}

void test_options_are_strict() {
    std::vector<std::string> storage = {"tool", "list", "--catalog", "dir", "--verify"};
    std::vector<char*> argv;
    for (auto& text : storage) {
        argv.push_back(text.data());
    }
    const cli::Options options(static_cast<int>(argv.size()), argv.data(), 2, {"--catalog"}, {"--verify"});
    REQUIRE(options.required("--catalog") == "dir");
    REQUIRE(options.flag("--verify"));
    const auto rejects = [](std::vector<std::string> arguments) {
        std::vector<char*> pointers;
        for (auto& text : arguments) {
            pointers.push_back(text.data());
        }
        REQUIRE_THROWS_AS(
            cli::Options(static_cast<int>(pointers.size()), pointers.data(), 2, {"--catalog"}, {"--verify"}),
            cli::UsageError
        );
    };
    rejects({"tool", "list", "--catalog", "a", "--catalog", "b"});
    rejects({"tool", "list", "--unknown", "a"});
    rejects({"tool", "list", "--catalog"});
    rejects({"tool", "list", "stray"});
    REQUIRE_THROWS_AS(static_cast<void>(options.required("--dataset")), cli::UsageError);
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"output text escapes what JSON cannot carry", test_output_text_escapes_what_json_cannot_carry},
        {"options are strict", test_options_are_strict},
    });
}
