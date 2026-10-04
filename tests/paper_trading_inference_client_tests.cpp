// The inference worker client (ADR 0018): strict parsing of every line,
// and real child processes: shell scripts stand in for the Python worker to
// exercise timeouts, exits, garbage, process-group cleanup, and the
// worker's environment. With CHRONOS_TEST_RESEARCH_PYTHON,
// CHRONOS_TEST_RESEARCH_ROOT, CHRONOS_TEST_RESEARCH_ARTIFACTS, and
// CHRONOS_TEST_RESEARCH_EXPERIMENT set, the real worker is also served.
#include "chronos/paper_trading/deployment_inference.hpp"
#include "test_support.hpp"

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace {

using chronos::InstrumentId;
using chronos::MarketDataStreamId;
using chronos::paper_trading::InferenceFailure;
using chronos::paper_trading::InferenceFailureCode;
using chronos::paper_trading::InferencePrediction;
using chronos::paper_trading::InferenceRequest;
using chronos::paper_trading::InferenceSession;
using chronos::paper_trading::ModelIdentity;
using chronos::paper_trading::ModelSelection;
using chronos::paper_trading::parse_prediction_line;
using chronos::paper_trading::parse_ready_line;
using chronos::paper_trading::PythonInferenceProvider;
using chronos::paper_trading::PythonWorkerCommand;
using chronos::paper_trading::request_line;
using chronos::strategy::PredictedClass;

const std::string kExperiment(64, 'e');

ModelSelection selection() {
    return ModelSelection{.experiment_id = kExperiment, .instrument = InstrumentId("AAPL"), .model = "logistic"};
}

std::string ready_line() {
    return R"({"experiment_id":")" + kExperiment +
        R"(","instrument":"AAPL","model":"logistic","model_feature_set":{"name":"top_of_book_baseline","version":"1"},)"
        R"("parameters_sha256":")" + std::string(64, 'f') +
        R"(","protocol":"chronos.inference_worker/1","training_cutoff_ns":"1790343319090266460","type":"ready"})";
}

InferenceRequest request(std::uint64_t id = 1) {
    return InferenceRequest{
        .id = id,
        .stream = MarketDataStreamId("s"),
        .instrument = InstrumentId("AAPL"),
        .ingest_sequence = 7,
        .inputs = {10'000'000, -5, 0, 100'000'000, 200'000'000},
    };
}

std::string prediction_line(std::string_view id = "1", std::string_view confidence = "611999999") {
    return R"({"class":"up","confidence_ppb":")" + std::string(confidence) + R"(","id":")" + std::string(id) +
        R"(","ingest_sequence":"7","instrument":"AAPL","probabilities":["0.1","0.288","0.612"],"stream":"s","type":"prediction"})";
}

InferenceFailureCode failure_code(const auto& result) {
    REQUIRE(std::holds_alternative<InferenceFailure>(result));
    return std::get<InferenceFailure>(result).code;
}

void test_the_ready_line_is_parsed_strictly() {
    const auto identity = parse_ready_line(ready_line(), selection());
    REQUIRE(std::holds_alternative<ModelIdentity>(identity));
    const auto& model = std::get<ModelIdentity>(identity);
    REQUIRE(model.parameters_sha256 == std::string(64, 'f'));
    REQUIRE(model.model_feature_set == "top_of_book_baseline/1");
    REQUIRE(model.training_cutoff.nanoseconds() == 1'790'343'319'090'266'460);

    std::string extra = ready_line();
    extra.insert(1, R"("extra":"1",)");
    REQUIRE(failure_code(parse_ready_line(extra, selection())) == InferenceFailureCode::ProtocolError);
    std::string protocol = ready_line();
    protocol.replace(protocol.find("worker/1"), 8, "worker/2");
    REQUIRE(failure_code(parse_ready_line(protocol, selection())) == InferenceFailureCode::ProtocolError);
    std::string leading_zero = ready_line();
    leading_zero.replace(leading_zero.find("\"1790"), 5, "\"01790");
    REQUIRE(failure_code(parse_ready_line(leading_zero, selection())) == InferenceFailureCode::ProtocolError);
    ModelSelection other = selection();
    other.model = "majority";
    REQUIRE(failure_code(parse_ready_line(ready_line(), other)) == InferenceFailureCode::ProtocolError);
    REQUIRE(failure_code(parse_ready_line(R"({"id":null,"message":"unevaluable","type":"error"})", selection())) ==
        InferenceFailureCode::ModelRejected);
    REQUIRE(failure_code(parse_ready_line("not json", selection())) == InferenceFailureCode::ProtocolError);
}

void test_predictions_must_answer_their_request() {
    REQUIRE(request_line(request()) ==
        R"({"id":"1","ingest_sequence":"7","inputs":["10000000","-5","0","100000000","200000000"],)"
        R"("instrument":"AAPL","stream":"s"})");
    const auto parsed = parse_prediction_line(prediction_line(), request());
    REQUIRE(std::holds_alternative<InferencePrediction>(parsed));
    REQUIRE(std::get<InferencePrediction>(parsed).predicted_class == PredictedClass::Up);
    REQUIRE(std::get<InferencePrediction>(parsed).confidence_ppb == 611'999'999);
    REQUIRE(std::get<InferencePrediction>(parsed).probabilities[2] == "0.612");
    REQUIRE(failure_code(parse_prediction_line(prediction_line("2"), request())) == InferenceFailureCode::ProtocolError);
    REQUIRE(failure_code(parse_prediction_line(prediction_line("1", "1000000001"), request())) ==
        InferenceFailureCode::ProtocolError);
    std::string quoted = prediction_line();
    quoted.replace(quoted.find("0.288"), 5, "0\\\"28");
    REQUIRE(failure_code(parse_prediction_line(quoted, request())) == InferenceFailureCode::ProtocolError);
    REQUIRE(failure_code(parse_prediction_line(R"({"id":"1","message":"bad inputs","type":"error"})", request())) ==
        InferenceFailureCode::RequestRefused);
    REQUIRE(failure_code(parse_prediction_line(R"({"id":"9","message":"bad inputs","type":"error"})", request())) ==
        InferenceFailureCode::ProtocolError);
}

void test_invalid_or_inconsistent_probabilities_are_refused() {
    for (const std::string bad : {"-0.1", "1.288", "1e999", "--", "0.2"}) {
        auto line = prediction_line();
        line.replace(line.find("0.288"), 5, bad);
        REQUIRE(failure_code(parse_prediction_line(line, request())) == InferenceFailureCode::ProtocolError);
    }
    REQUIRE(failure_code(parse_prediction_line(prediction_line("1", "612000000"), request())) ==
        InferenceFailureCode::ProtocolError);
    auto wrong_class = prediction_line();
    wrong_class.replace(wrong_class.find("\"up\""), 4, "\"down\"");
    REQUIRE(failure_code(parse_prediction_line(wrong_class, request())) == InferenceFailureCode::ProtocolError);
    auto duplicate = prediction_line();
    duplicate.insert(1, "\"class\":\"down\",");
    REQUIRE(failure_code(parse_prediction_line(duplicate, request())) == InferenceFailureCode::ProtocolError);
}

// A shell script in place of the Python interpreter. It ignores its
// arguments, which are `-m chronos_research inference-worker ...`.
class FakeWorker final {
public:
    explicit FakeWorker(const std::string& body)
        : directory_(std::filesystem::temp_directory_path() /
              ("chronos-fake-worker-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++))) {
        std::filesystem::create_directories(directory_);
        script_ = directory_ / "python";
        std::ofstream(script_) << "#!/bin/sh\n" << body << "\n";
        std::filesystem::permissions(script_, std::filesystem::perms::owner_all);
    }
    ~FakeWorker() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
    FakeWorker(const FakeWorker&) = delete;
    FakeWorker& operator=(const FakeWorker&) = delete;

    [[nodiscard]] PythonInferenceProvider provider() const {
        return PythonInferenceProvider(PythonWorkerCommand{
            .python = script_,
            .python_root = directory_,
            .artifacts = directory_,
        });
    }
    [[nodiscard]] std::filesystem::path file(std::string_view name) const { return directory_ / name; }

private:
    static inline int counter_ = 0;
    std::filesystem::path directory_;
    std::filesystem::path script_;
};

std::string single_quoted(const std::string& text) {
    std::string quoted = "'";
    for (char character : text) {
        quoted += character == '\'' ? std::string("'\\''") : std::string(1, character);
    }
    return quoted + "'";
}

std::unique_ptr<InferenceSession> open(PythonInferenceProvider& provider) {
    auto opened = provider.open(selection(), std::chrono::milliseconds(5'000));
    if (const auto* failed = std::get_if<InferenceFailure>(&opened)) {
        throw chronos::test::TestFailure("open failed: " + failed->message);
    }
    return std::get<std::unique_ptr<InferenceSession>>(std::move(opened));
}

void test_a_worker_is_served_line_by_line() {
    const FakeWorker worker(
        "printf '%s\\n' " + single_quoted(ready_line()) + "\n"
        "while read -r line; do printf '%s\\n' " + single_quoted(prediction_line()) + "; done"
    );
    auto provider = worker.provider();
    const auto described = provider.describe(selection(), std::chrono::milliseconds(5'000));
    REQUIRE(std::holds_alternative<ModelIdentity>(described));
    auto session = open(provider);
    for (int index = 0; index < 3; ++index) {
        REQUIRE(std::holds_alternative<InferencePrediction>(session->predict(request(), std::chrono::milliseconds(2'000))));
    }
}

void test_oversized_requests_are_refused_before_writing() {
    const FakeWorker worker("printf '%s\\n' " + single_quoted(ready_line()) + "\nsleep 2");
    auto provider = worker.provider();
    auto session = open(provider);
    auto oversized = request();
    oversized.stream = MarketDataStreamId(std::string(100'000, 's'));
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(failure_code(session->predict(oversized, std::chrono::milliseconds(100))) ==
        InferenceFailureCode::ProtocolError);
    REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
}

void test_a_worker_that_never_drains_input_cannot_bypass_the_timeout() {
    // The watchdog makes this regression fail finitely on the old blocking
    // writer. Correctly echoed replies alone must not hide stdin backpressure.
    const FakeWorker worker(
        "(sleep 3; kill -KILL $$) &\n"
        "printf '%s\\n' " + single_quoted(ready_line()) + "\n"
        "while :; do printf '%s\\n' " + single_quoted(prediction_line()) + "; done"
    );
    auto provider = worker.provider();
    auto session = open(provider);
    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < 10'000; ++index) {
        const auto result = session->predict(request(), std::chrono::milliseconds(100));
        if (std::holds_alternative<InferenceFailure>(result)) {
            REQUIRE(failure_code(result) == InferenceFailureCode::Timeout);
            REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
            return;
        }
    }
    throw chronos::test::TestFailure("the writer never encountered the undrained input pipe");
}

bool process_exists(pid_t pid) { return ::kill(pid, 0) == 0; }

pid_t read_pid(const std::filesystem::path& file) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        std::ifstream input(file);
        pid_t pid = 0;
        if (input >> pid && pid > 0) {
            return pid;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw chronos::test::TestFailure("the fake worker did not record its child");
}

void test_a_timeout_ends_the_whole_worker_process_group() {
    const FakeWorker worker("");
    const FakeWorker sleeper(
        "sleep 300 &\n"
        "echo $! > " + single_quoted(worker.file("child").string()) + "\n"
        "printf '%s\\n' " + single_quoted(ready_line()) + "\n"
        "read -r line\n"
        "sleep 300"
    );
    auto provider = sleeper.provider();
    auto session = open(provider);
    const pid_t child = read_pid(worker.file("child"));
    REQUIRE(process_exists(child));
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(failure_code(session->predict(request(), std::chrono::milliseconds(200))) == InferenceFailureCode::Timeout);
    REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    // The session ended the worker and everything it started.
    for (int attempt = 0; attempt < 200 && process_exists(child); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(!process_exists(child));
    REQUIRE(failure_code(session->predict(request(), std::chrono::milliseconds(200))) == InferenceFailureCode::WorkerExited);
}

void test_an_exited_or_garbled_worker_fails_closed() {
    {
        const FakeWorker exits("printf '%s\\n' " + single_quoted(ready_line()) + "\nexit 0");
        auto provider = exits.provider();
        auto session = open(provider);
        const auto failed = session->predict(request(), std::chrono::milliseconds(2'000));
        REQUIRE(failure_code(failed) == InferenceFailureCode::WorkerExited);
    }
    {
        const FakeWorker garbled("printf 'hello\\n'");
        auto provider = garbled.provider();
        REQUIRE(failure_code(provider.open(selection(), std::chrono::milliseconds(2'000))) ==
            InferenceFailureCode::ProtocolError);
    }
    {
        const FakeWorker silent("sleep 300");
        auto provider = silent.provider();
        REQUIRE(failure_code(provider.open(selection(), std::chrono::milliseconds(200))) ==
            InferenceFailureCode::Timeout);
    }
    {
        const FakeWorker refuses(R"(printf '%s\n' '{"id":null,"message":"the model is unevaluable","type":"error"}')");
        auto provider = refuses.provider();
        REQUIRE(failure_code(provider.describe(selection(), std::chrono::milliseconds(2'000))) ==
            InferenceFailureCode::ModelRejected);
    }
    {
        const FakeWorker answers_wrongly(
            "printf '%s\\n' " + single_quoted(ready_line()) + "\n"
            "read -r line\nprintf '%s\\n' " + single_quoted(prediction_line("5"))
        );
        auto provider = answers_wrongly.provider();
        auto session = open(provider);
        REQUIRE(failure_code(session->predict(request(), std::chrono::milliseconds(2'000))) ==
            InferenceFailureCode::ProtocolError);
    }
    PythonInferenceProvider missing(PythonWorkerCommand{
        .python = "/nonexistent/python", .python_root = "/nonexistent", .artifacts = "/nonexistent",
    });
    REQUIRE(failure_code(missing.open(selection(), std::chrono::milliseconds(200))) == InferenceFailureCode::SpawnFailed);
}

void test_the_worker_gets_none_of_the_servers_environment() {
    ::setenv("CHRONOS_TEST_SECRET_CANARY", "do-not-leak", 1);
    const FakeWorker worker("");
    const FakeWorker printer(
        "env > " + single_quoted(worker.file("environment").string()) + "\n"
        "printf '%s\\n' " + single_quoted(ready_line())
    );
    auto provider = printer.provider();
    REQUIRE(std::holds_alternative<ModelIdentity>(provider.describe(selection(), std::chrono::milliseconds(2'000))));
    std::ifstream input(worker.file("environment"));
    std::string environment((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(environment.find("PYTHONPATH=") != std::string::npos);
    REQUIRE(environment.find("CHRONOS_TEST_SECRET_CANARY") == std::string::npos);
    REQUIRE(environment.find("APCA_") == std::string::npos);
    REQUIRE(environment.find("CHRONOS_POSTGRES_URL") == std::string::npos);
    ::unsetenv("CHRONOS_TEST_SECRET_CANARY");
}

// The real worker on a real experiment, when one is configured.
void test_the_python_worker_serves_a_verified_model() {
    const char* python = std::getenv("CHRONOS_TEST_RESEARCH_PYTHON");
    const char* root = std::getenv("CHRONOS_TEST_RESEARCH_ROOT");
    const char* artifacts = std::getenv("CHRONOS_TEST_RESEARCH_ARTIFACTS");
    const char* experiment = std::getenv("CHRONOS_TEST_RESEARCH_EXPERIMENT");
    if (python == nullptr || root == nullptr || artifacts == nullptr || experiment == nullptr) {
        std::cout << "  (no research environment configured; real worker not exercised)\n";
        return;
    }
    PythonInferenceProvider provider(PythonWorkerCommand{.python = python, .python_root = root, .artifacts = artifacts});
    const ModelSelection model{.experiment_id = experiment, .instrument = InstrumentId("AAPL"), .model = "logistic"};
    auto opened = provider.open(model, std::chrono::milliseconds(60'000));
    if (const auto* failed = std::get_if<InferenceFailure>(&opened)) {
        throw chronos::test::TestFailure("the real worker failed: " + failed->message);
    }
    auto session = std::get<std::unique_ptr<InferenceSession>>(std::move(opened));
    REQUIRE(session->identity().experiment_id == experiment);
    REQUIRE(session->identity().model_feature_set == "top_of_book_baseline/1");
    InferenceRequest asked = request();
    const auto first = session->predict(asked, std::chrono::milliseconds(5'000));
    asked.id = 2;
    const auto second = session->predict(asked, std::chrono::milliseconds(5'000));
    REQUIRE(std::holds_alternative<InferencePrediction>(first));
    REQUIRE(std::get<InferencePrediction>(first).probabilities == std::get<InferencePrediction>(second).probabilities);
    if (const char* expected = std::getenv("CHRONOS_TEST_RESEARCH_PARAMETERS_SHA256")) {
        REQUIRE(session->identity().parameters_sha256 == expected);
    }
}

}  // namespace

int main() {
    // As the server does whenever deployments are enabled.
    ::signal(SIGPIPE, SIG_IGN);
    return chronos::test::run_tests({
        {"the ready line is parsed strictly", test_the_ready_line_is_parsed_strictly},
        {"predictions must answer their request", test_predictions_must_answer_their_request},
        {"invalid or inconsistent probabilities are refused", test_invalid_or_inconsistent_probabilities_are_refused},
        {"a worker is served line by line", test_a_worker_is_served_line_by_line},
        {"oversized requests are refused before writing", test_oversized_requests_are_refused_before_writing},
        {"an undrained input pipe cannot bypass the timeout", test_a_worker_that_never_drains_input_cannot_bypass_the_timeout},
        {"a timeout ends the whole worker process group", test_a_timeout_ends_the_whole_worker_process_group},
        {"an exited or garbled worker fails closed", test_an_exited_or_garbled_worker_fails_closed},
        {"the worker gets none of the server's environment", test_the_worker_gets_none_of_the_servers_environment},
        {"the Python worker serves a verified model", test_the_python_worker_serves_a_verified_model},
    });
}
