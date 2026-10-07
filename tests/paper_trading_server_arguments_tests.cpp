#include "chronos/paper_trading/server_arguments.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

using chronos::InstrumentId;
using chronos::ReplaySpeed;
using chronos::paper_trading::parse_server_arguments;
using chronos::paper_trading::server_usage;
using chronos::paper_trading::ServerArguments;
using chronos::paper_trading::ServerCapabilities;
using chronos::paper_trading::ServerUsageError;

constexpr ServerCapabilities kWithReplay{.databento_replay = true, .alpaca_live = true};
constexpr ServerCapabilities kWithoutReplay{.databento_replay = false, .alpaca_live = true};
constexpr ServerCapabilities kWithoutAlpaca{.databento_replay = true, .alpaca_live = false};

ServerArguments parse(
    std::initializer_list<std::string_view> arguments,
    ServerCapabilities capabilities = kWithReplay
) {
    const std::vector<std::string_view> list(arguments);
    return parse_server_arguments(list, capabilities);
}

// The usage error's message for an invalid command line.
std::string rejection(
    std::initializer_list<std::string_view> arguments,
    ServerCapabilities capabilities = kWithReplay
) {
    try {
        static_cast<void>(parse(arguments, capabilities));
    } catch (const ServerUsageError& error) {
        return error.what();
    }
    throw chronos::test::TestFailure("expected a usage error");
}

void test_minimal_command_line_has_no_source() {
    const auto parsed = parse({"--stream-id", "s", "--instrument", "AAPL", "--instrument", "SPY"});
    REQUIRE(parsed.stream_id == "s");
    REQUIRE((parsed.instruments == std::vector{InstrumentId("AAPL"), InstrumentId("SPY")}));
    REQUIRE(!parsed.replay.has_value());
    REQUIRE(!parsed.alpaca_iex);
    REQUIRE(parsed.http.host == "127.0.0.1");
    REQUIRE(parsed.http.port == 8080);
    REQUIRE(parsed.risk_limits.maximum_quote_age_nanoseconds == 5'000'000'000);
}

void test_replay_defaults_to_ready_at_real_time() {
    const auto parsed = parse({"--stream-id", "s", "--instrument", "AAPL", "--replay-dbn", "r.dbn.zst"});
    REQUIRE(parsed.replay.has_value());
    REQUIRE(parsed.replay->recording == "r.dbn.zst");
    REQUIRE(parsed.replay->speed == ReplaySpeed::RealTime);
    REQUIRE(!parsed.replay->start_immediately);
}

void test_replay_speed_and_autostart_are_parsed() {
    for (const auto& [name, speed] : {
             std::pair{"1x", ReplaySpeed::RealTime},
             std::pair{"10x", ReplaySpeed::TenTimes},
             std::pair{"100x", ReplaySpeed::HundredTimes},
             std::pair{"max", ReplaySpeed::Maximum},
         }) {
        const auto parsed = parse({
            "--stream-id", "s", "--instrument", "AAPL",
            "--replay-dbn", "r", "--replay-speed", name, "--replay-autostart",
        });
        REQUIRE(parsed.replay->speed == speed);
        REQUIRE(parsed.replay->start_immediately);
    }
}

void test_invalid_speeds_are_rejected() {
    for (const auto* speed : {"2x", "1.5", "MAX", "", "fast"}) {
        REQUIRE(rejection({
            "--stream-id", "s", "--instrument", "AAPL",
            "--replay-dbn", "r", "--replay-speed", speed,
        }) == "--replay-speed must be one of 1x, 10x, 100x, max");
    }
}

void test_replay_options_require_a_recording() {
    REQUIRE(rejection({"--stream-id", "s", "--instrument", "AAPL", "--replay-speed", "10x"}) ==
            "--replay-speed requires --replay-dbn");
    REQUIRE(rejection({"--stream-id", "s", "--instrument", "AAPL", "--replay-autostart"}) ==
            "--replay-autostart requires --replay-dbn");
}

void test_replay_requires_a_databento_build() {
    REQUIRE(rejection(
        {"--stream-id", "s", "--instrument", "AAPL", "--replay-dbn", "r"},
        kWithoutReplay
    ) == "--replay-dbn requires a build with CHRONOS_ENABLE_DATABENTO_ADAPTER=ON");
}

void test_options_may_not_repeat() {
    for (const auto* option : {"--stream-id", "--host", "--replay-dbn", "--replay-speed"}) {
        const std::string value = std::string_view(option) == "--replay-speed" ? "1x" : "x";
        REQUIRE(rejection({
            "--stream-id", "s", "--instrument", "AAPL", "--replay-dbn", "r",
            option, value, option, value,
        }) == std::string(option) + " may be provided only once");
    }
    REQUIRE(rejection({
        "--stream-id", "s", "--instrument", "AAPL", "--replay-dbn", "r",
        "--replay-autostart", "--replay-autostart",
    }) == "--replay-autostart may be provided only once");
    REQUIRE(rejection({"--stream-id", "s", "--instrument", "AAPL", "--instrument", "AAPL"}) ==
            "--instrument AAPL is listed more than once");
}

void test_malformed_command_lines_are_rejected() {
    REQUIRE(rejection({"--instrument", "AAPL"}) ==
            "--stream-id and at least one --instrument are required");
    REQUIRE(rejection({"--stream-id", "s"}) ==
            "--stream-id and at least one --instrument are required");
    REQUIRE(rejection({"--stream-id"}) == "--stream-id requires a value");
    REQUIRE(rejection({"--stream-id", "s", "--instrument", ""}) == "--instrument requires a symbol");
    REQUIRE(rejection({"--stream-id", "s", "--instrument", "AAPL", "--port", "0"}) ==
            "--port must be an integer from 1 to 65535");
    REQUIRE(rejection({"--stream-id", "s", "--instrument", "AAPL", "--api-key", "x"}) ==
            "unknown option: --api-key");
    REQUIRE(rejection({"--help"}).empty());
}

void test_usage_describes_every_source_option() {
    const std::string usage = server_usage();
    for (const auto* option : {
             "--replay-dbn", "--replay-speed", "--replay-autostart", "--stream-id", "--alpaca-iex",
             "APCA_API_KEY_ID", "APCA_API_SECRET_KEY", "not SIP or NBBO",
         }) {
        REQUIRE(usage.find(option) != std::string::npos);
    }
    REQUIRE(usage.find("Simulated trading only") != std::string::npos);
}

void test_alpaca_iex_selects_the_live_source() {
    const auto parsed = parse({"--stream-id", "live", "--instrument", "AAPL", "--instrument", "SPY", "--alpaca-iex"});
    REQUIRE(parsed.alpaca_iex);
    REQUIRE(!parsed.replay.has_value());
    REQUIRE((parsed.instruments == std::vector{InstrumentId("AAPL"), InstrumentId("SPY")}));
}

// Adopting a live stream's pre-006 history is explicit, and only for a
// live source (ADR 0020).
void test_adopting_live_stream_history_is_explicit_and_live_only() {
    REQUIRE(!parse({"--stream-id", "live", "--instrument", "AAPL", "--alpaca-iex"}).adopt_live_stream_history);
    REQUIRE(parse({
        "--stream-id", "live", "--instrument", "AAPL", "--alpaca-iex", "--adopt-live-stream-history",
    }).adopt_live_stream_history);
    REQUIRE(rejection({"--stream-id", "s", "--instrument", "AAPL", "--adopt-live-stream-history"}) ==
            "--adopt-live-stream-history requires --alpaca-iex");
    REQUIRE(rejection({
        "--stream-id", "s", "--instrument", "AAPL", "--replay-dbn", "r", "--adopt-live-stream-history",
    }) == "--adopt-live-stream-history requires --alpaca-iex");
    REQUIRE(rejection({
        "--stream-id", "s", "--instrument", "AAPL", "--alpaca-iex", "--adopt-live-stream-history",
        "--adopt-live-stream-history",
    }) == "--adopt-live-stream-history may be provided only once");
    REQUIRE(chronos::paper_trading::server_usage().find("--adopt-live-stream-history") != std::string::npos);
}

void test_alpaca_iex_is_exclusive_strict_and_limited() {
    REQUIRE(rejection({
        "--stream-id", "s", "--instrument", "AAPL", "--alpaca-iex", "--replay-dbn", "r",
    }) == "choose one market-data source: --replay-dbn or --alpaca-iex");
    REQUIRE(rejection({
        "--stream-id", "s", "--instrument", "AAPL", "--alpaca-iex", "--replay-speed", "10x",
    }) == "--replay-speed requires --replay-dbn");
    REQUIRE(rejection({
        "--stream-id", "s", "--instrument", "AAPL", "--alpaca-iex", "--replay-autostart",
    }) == "--replay-autostart requires --replay-dbn");
    REQUIRE(rejection(
        {"--stream-id", "s", "--instrument", "AAPL", "--alpaca-iex"},
        kWithoutAlpaca
    ) == "--alpaca-iex requires a build with CHRONOS_ENABLE_ALPACA_ADAPTER=ON");
    REQUIRE(rejection({"--stream-id", "s", "--instrument", "AAPL", "--alpaca-iex", "--alpaca-iex"}) ==
            "--alpaca-iex may be provided only once");

    std::vector<std::string> symbols;
    for (int index = 0; index < 31; ++index) {
        symbols.push_back("S" + std::to_string(index));
    }
    std::vector<std::string_view> arguments{"--stream-id", "s", "--alpaca-iex"};
    for (std::size_t index = 0; index < 30; ++index) {
        arguments.push_back("--instrument");
        arguments.push_back(symbols[index]);
    }
    REQUIRE(parse_server_arguments(arguments, kWithReplay).instruments.size() == 30U);
    arguments.push_back("--instrument");
    arguments.push_back(symbols[30]);
    try {
        static_cast<void>(parse_server_arguments(arguments, kWithReplay));
        throw chronos::test::TestFailure("expected a usage error");
    } catch (const ServerUsageError& error) {
        REQUIRE(std::string(error.what()) ==
            "--alpaca-iex supports at most 30 instruments, the limit of Alpaca's Basic plan");
    }
}

void test_credentials_are_never_command_line_options() {
    for (const auto* option : {
             "--alpaca-key", "--alpaca-secret", "--apca-api-key-id", "--apca-api-secret-key", "--key",
         }) {
        REQUIRE(rejection({"--stream-id", "s", "--instrument", "AAPL", "--alpaca-iex", option, "x"}) ==
                "unknown option: " + std::string(option));
    }
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"minimal command line has no source", test_minimal_command_line_has_no_source},
        {"replay defaults to ready at real time", test_replay_defaults_to_ready_at_real_time},
        {"replay speed and autostart are parsed", test_replay_speed_and_autostart_are_parsed},
        {"invalid speeds are rejected", test_invalid_speeds_are_rejected},
        {"replay options require a recording", test_replay_options_require_a_recording},
        {"replay requires a Databento build", test_replay_requires_a_databento_build},
        {"options may not repeat", test_options_may_not_repeat},
        {"malformed command lines are rejected", test_malformed_command_lines_are_rejected},
        {"usage describes every source option", test_usage_describes_every_source_option},
        {"--alpaca-iex selects the live source", test_alpaca_iex_selects_the_live_source},
        {"--alpaca-iex is exclusive, strict, and limited", test_alpaca_iex_is_exclusive_strict_and_limited},
        {
            "adopting live stream history is explicit and live only",
            test_adopting_live_stream_history_is_explicit_and_live_only,
        },
        {"credentials are never command-line options", test_credentials_are_never_command_line_options},
    });
}
