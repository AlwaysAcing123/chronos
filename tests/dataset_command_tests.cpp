#include "artifact_test_support.hpp"
#include "chronos/canonical_json.hpp"
#include "chronos/sha256.hpp"
#include "dataset_test_support.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <sys/wait.h>

// Runs the chronos_dataset executable itself, whose path is the first
// argument, so option parsing, exit codes, and output are tested as a
// person or script sees them.
namespace {

namespace json = chronos::canonical_json;
using chronos::test::TemporaryDirectory;

std::string binary;

struct Outcome {
    int status;
    std::string output;
};

std::string shell_quoted(const std::filesystem::path& path) { return "'" + path.string() + "'"; }

Outcome run(const std::string& arguments) {
    const std::string command = shell_quoted(binary) + " " + arguments + " 2>&1";
    FILE* pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::string output;
    char buffer[4096];
    while (const std::size_t read = std::fread(buffer, 1, sizeof buffer, pipe)) {
        output.append(buffer, read);
    }
    const int status = ::pclose(pipe);
    REQUIRE(WIFEXITED(status));
    return Outcome{.status = WEXITSTATUS(status), .output = std::move(output)};
}

void rejects(const std::string& arguments, std::string_view message) {
    const Outcome outcome = run(arguments);
    if (outcome.status != 2 || outcome.output.find(message) == std::string::npos) {
        throw chronos::test::TestFailure("`" + arguments + "` gave " + std::to_string(outcome.status) + ": " + outcome.output);
    }
}

// The value at a path of keys in a command's JSON output.
std::string field(const Outcome& outcome, std::initializer_list<std::string_view> path) {
    REQUIRE(outcome.status == 0);
    auto parsed = json::parse(outcome.output.substr(0, outcome.output.find_last_not_of('\n') + 1));
    REQUIRE(std::holds_alternative<json::Value>(parsed));
    const json::Value* value = &std::get<json::Value>(parsed);
    for (const std::string_view key : path) {
        const json::Object* object = value->if_object();
        REQUIRE(object != nullptr);
        const json::Value* found = nullptr;
        for (const auto& member : *object) {
            if (member.first == key) {
                found = &member.second;
            }
        }
        REQUIRE(found != nullptr);
        value = found;
    }
    REQUIRE(value->if_string() != nullptr);
    return *value->if_string();
}

void test_malformed_label_commands_are_refused() {
    rejects("labels", "labels needs a command");
    rejects("labels frobnicate", "unknown labels command: frobnicate");
    rejects("labels extract --horizon-ns 1 --horizon-ns 2", "--horizon-ns may be provided only once");
    rejects("labels extract --horizon 5", "unknown option: --horizon");
    rejects("labels extract stray", "unknown option: stray");
    rejects("labels inspect --output", "--output requires a value");
    rejects("labels extract --horizon-ns ''", "--horizon-ns requires a value");
    rejects("labels inspect --output --artifact " + std::string(64, 'a'), "--output requires a value");
    rejects("labels inspect --output o --artifact " + std::string(64, 'a') + " --features ''", "--features requires a value");
    for (const std::string horizon : {"0", "-1", "01", "1e9", "1s", "+5", "9223372036854775808"}) {
        rejects("labels extract --horizon-ns " + horizon, "--horizon-ns must be a whole number from 1 to 9223372036854775807");
    }
    rejects("labels extract --label-set future_midpoint_return_v2", "the only label set is future_midpoint_return_v1");
    rejects("labels extract --horizon-ns 5", "--features is required");
    rejects("labels extract --features f --feature-artifact abc --output o", "--feature-artifact must be a 64-digit lowercase ID");
    rejects("labels inspect --output o --artifact " + std::string(64, 'A'), "--artifact must be a 64-digit lowercase ID");
    rejects("labels list", "--output is required");
}

void test_malformed_split_commands_are_refused() {
    rejects("splits", "splits needs a command");
    rejects("splits shuffle", "unknown splits command: shuffle");
    rejects("splits create --train-percent 50 --train-percent 60", "--train-percent may be provided only once");
    rejects("splits create --percent 50", "unknown option: --percent");
    rejects("splits create --train-percent 70", "must add up to exactly 100");
    rejects("splits create --train-percent 50 --validation-percent 30 --test-percent 30", "must add up to exactly 100");
    for (const std::string percent : {"0", "99", "100", "-20", "020", "20.0", "20%"}) {
        rejects("splits create --test-percent " + percent, "--test-percent must be a whole number from 1 to 98");
    }
    rejects("splits create --split-set random_v1", "the only split set is chronological_purged_v1");
    rejects("splits create", "--labels is required");
    rejects("splits create --labels l --label-artifact xyz --output o", "--label-artifact must be a 64-digit lowercase ID");
    rejects("splits inspect --output o --artifact xyz", "--artifact must be a 64-digit lowercase ID");
    rejects("splits list --output o --output p", "--output may be provided only once");
}

void test_the_pipeline_round_trips_through_the_command_line() {
    const TemporaryDirectory scratch;
    const std::filesystem::path catalog = scratch.path() / "catalog";
    std::filesystem::create_directory(catalog);
    // Twenty AAPL quotes a microsecond apart, alternating between two
    // midpoints.
    std::vector<chronos::ValidatedMarketEvent> events;
    for (std::uint64_t index = 0; index < 20; ++index) {
        const std::int64_t at = static_cast<std::int64_t>(index + 1) * 1'000;
        events.push_back(index % 2 == 0 ? chronos::test::two_sided("AAPL", index, at, "100", "100.02")
                                        : chronos::test::two_sided("AAPL", index, at, "100.02", "100.04"));
    }
    const auto dataset = chronos::test::published(chronos::test::write_dataset(catalog, events));
    const std::filesystem::path features = scratch.path() / "features";
    const std::filesystem::path labels = scratch.path() / "labels";
    const std::filesystem::path splits = scratch.path() / "splits";
    const std::string feature_id = field(run(
        "features extract --catalog " + shell_quoted(catalog) + " --dataset " + dataset.manifest.dataset_id +
        " --output " + shell_quoted(features)
    ), {"artifact", "artifact_id"});

    const std::string extract = "labels extract --features " + shell_quoted(features) + " --feature-artifact " + feature_id +
        " --output " + shell_quoted(labels) + " --horizon-ns 1000";
    const Outcome extracted = run(extract);
    REQUIRE(field(extracted, {"outcome"}) == "published");
    REQUIRE(field(extracted, {"artifact", "rows"}) == "20");
    REQUIRE(field(extracted, {"artifact", "usable"}) == "19");
    REQUIRE(field(extracted, {"artifact", "directions", "up"}) == "10");
    REQUIRE(field(extracted, {"artifact", "directions", "down"}) == "9");
    REQUIRE(field(extracted, {"artifact", "unavailable", "no_future_observation"}) == "1");
    REQUIRE(field(extracted, {"artifact", "source", "feature_artifact_id"}) == feature_id);
    const std::string label_id = field(extracted, {"artifact", "artifact_id"});
    REQUIRE(field(run(extract), {"outcome"}) == "already_present");
    REQUIRE(field(run("labels inspect --output " + shell_quoted(labels) + " --artifact " + label_id + " --features " +
        shell_quoted(features)), {"reproduced"}) == "recomputed from the source feature artifact: identical rows");
    REQUIRE(field(run("labels inspect --output " + shell_quoted(labels) + " --artifact " + label_id), {"reproduced"})
        .starts_with("not checked"));
    REQUIRE(run("labels list --output " + shell_quoted(labels)).output.find(label_id) != std::string::npos);

    // Each label is known when the next quote arrives, one period-start
    // later for the last row of train and of validation, which are purged.
    const std::string create = "splits create --labels " + shell_quoted(labels) + " --label-artifact " + label_id +
        " --output " + shell_quoted(splits);
    const Outcome split = run(create);
    REQUIRE(field(split, {"outcome"}) == "published");
    REQUIRE(field(split, {"artifact", "source", "label_artifact_id"}) == label_id);
    REQUIRE(field(split, {"artifact", "boundaries", "validation_start", "row"}) == "12");
    REQUIRE(field(split, {"artifact", "boundaries", "test_start", "row"}) == "16");
    REQUIRE(field(split, {"artifact", "usable", "train", "rows"}) == "11");
    REQUIRE(field(split, {"artifact", "usable", "validation", "rows"}) == "3");
    REQUIRE(field(split, {"artifact", "usable", "test", "rows"}) == "3");
    REQUIRE(field(split, {"artifact", "counts", "excluded", "label_crosses_validation_start"}) == "1");
    REQUIRE(field(split, {"artifact", "counts", "excluded", "label_crosses_test_start"}) == "1");
    REQUIRE(field(split, {"artifact", "counts", "excluded", "label_unavailable"}) == "1");
    const std::string split_id = field(split, {"artifact", "artifact_id"});
    REQUIRE(field(run(create), {"outcome"}) == "already_present");
    REQUIRE(field(run("splits inspect --output " + shell_quoted(splits) + " --artifact " + split_id + " --labels " +
        shell_quoted(labels)), {"reproduced"}) == "recomputed from the source label artifact: identical rows");
    REQUIRE(run("splits list --output " + shell_quoted(splits)).output.find(split_id) != std::string::npos);
    REQUIRE(field(run(create + " --train-percent 50 --validation-percent 25 --test-percent 25"),
        {"artifact", "artifact_id"}) != split_id);

    // Failures are one JSON error document and exit 1.
    const Outcome missing = run("labels inspect --output " + shell_quoted(labels) + " --artifact " + std::string(64, 'f'));
    REQUIRE(missing.status == 1);
    REQUIRE(missing.output.find("\"code\": \"not_found\"") != std::string::npos);
    const Outcome lopsided = run(create + " --train-percent 98 --validation-percent 1 --test-percent 1");
    REQUIRE(lopsided.status == 1);
    REQUIRE(lopsided.output.find("\"code\": \"empty_split\"") != std::string::npos);
}

// The production synthetic producer (ADR 0025): a scenario's event table
// becomes a dataset whose provenance says it is synthetic, checksummed from
// the table's own bytes, published once and found again after.
void test_a_synthetic_scenario_is_published_with_its_provenance() {
    const TemporaryDirectory scratch;
    const std::filesystem::path table = scratch.path() / "scenario.csv";
    const std::string rows =
        "# invented quotes\n"
        "status,AAPL,1000,open\n"
        "quote,AAPL,1100,100.00,3,100.02,1\n"
        "quote,SPY,1100,500.00,10,500.01,30\n"
        "quote,AAPL,1200,100.01,2,,\n";
    chronos::test::write_file(table, rows);
    const std::string write = "synthetic write --events " + shell_quoted(table) + " --catalog " +
        shell_quoted(scratch.path() / "catalog") + " --stream-id demo-stream --scenario chronos.demo_scenario" +
        " --scenario-version 1";
    const Outcome written = run(write);
    REQUIRE(field(written, {"outcome"}) == "published");
    REQUIRE(field(written, {"dataset", "provider"}) == "synthetic");
    REQUIRE(field(written, {"dataset", "stream"}) == "demo-stream");
    REQUIRE(field(written, {"dataset", "counts", "events"}) == "4");
    REQUIRE(field(written, {"dataset", "checksums", "recording_sha256"}) == chronos::Sha256::hex(rows));
    REQUIRE(field(written, {"provenance", "normalizer"}) == "chronos.synthetic.event_table/1");
    REQUIRE(field(written, {"provenance", "description"}) ==
        "invented events of scenario chronos.demo_scenario version 1, not market data");
    REQUIRE(field(run(write), {"outcome"}) == "already_present");
    const std::string dataset_id = field(written, {"dataset", "dataset_id"});
    REQUIRE(field(run("inspect --catalog " + shell_quoted(scratch.path() / "catalog") + " --dataset " + dataset_id),
        {"verified"}) == "every event, the checksums, the summary, and the quality report");

    // Another version of the scenario is another dataset.
    REQUIRE(field(run(write.substr(0, write.size() - 1) + "2"), {"dataset", "dataset_id"}) != dataset_id);

    chronos::test::write_file(scratch.path() / "bad.csv", "quote,AAPL,1100,100.00,3\n");
    const Outcome bad = run("synthetic write --events " + shell_quoted(scratch.path() / "bad.csv") + " --catalog " +
        shell_quoted(scratch.path() / "other") + " --stream-id s --scenario a --scenario-version 1");
    REQUIRE(bad.status == 1);
    REQUIRE(bad.output.find("expected a quote with 7 fields or a status with 4") != std::string::npos);
    REQUIRE(bad.output.find("\"line\": \"1\"") != std::string::npos);
}

void test_malformed_synthetic_commands_are_refused() {
    rejects("synthetic", "synthetic needs a subcommand: write");
    rejects("synthetic generate", "synthetic needs a subcommand: write");
    rejects("synthetic write --events e", "--catalog is required");
    rejects("synthetic write --events e --catalog c --stream-id s --scenario Demo --scenario-version 1",
        "--scenario must be 1 to 64 lowercase letters");
    rejects("synthetic write --events e --catalog c --stream-id s --scenario demo --scenario-version 01",
        "--scenario-version must be a version");
    rejects("synthetic write --events e --events f", "--events may be provided only once");
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        return 2;
    }
    binary = argv[1];
    return chronos::test::run_tests({
        {"malformed label commands are refused", test_malformed_label_commands_are_refused},
        {"malformed split commands are refused", test_malformed_split_commands_are_refused},
        {"the pipeline round-trips through the command line", test_the_pipeline_round_trips_through_the_command_line},
        {
            "a synthetic scenario is published with its provenance",
            test_a_synthetic_scenario_is_published_with_its_provenance,
        },
        {"malformed synthetic commands are refused", test_malformed_synthetic_commands_are_refused},
    });
}
