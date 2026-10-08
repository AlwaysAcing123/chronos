#include "chronos/paper_trading/deployment_inference.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace chronos::paper_trading {

std::string_view inference_failure_name(InferenceFailureCode code) noexcept {
    switch (code) {
        case InferenceFailureCode::SpawnFailed: return "inference_spawn_failed";
        case InferenceFailureCode::ModelRejected: return "model_rejected";
        case InferenceFailureCode::Timeout: return "inference_timeout";
        case InferenceFailureCode::WorkerExited: return "inference_worker_exited";
        case InferenceFailureCode::ProtocolError: return "inference_protocol_error";
        case InferenceFailureCode::RequestRefused: return "inference_request_refused";
    }
    return "inference_protocol_error";
}

namespace {

using Json = nlohmann::json;

// Longer than any ready or prediction line the protocol allows.
constexpr std::size_t kMaximumReplyBytes = 64 * 1024;
constexpr std::size_t kMaximumRequestBytes = 8 * 1024;

InferenceFailure failure(InferenceFailureCode code, std::string message) {
    return InferenceFailure{.code = code, .message = std::move(message)};
}

std::optional<Json> parse_object(std::string_view line) {
    std::vector<std::set<std::string>> keys;
    bool duplicate = false;
    const Json::parser_callback_t callback = [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) {
            keys.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            keys.pop_back();
        } else if (event == Json::parse_event_t::key && !keys.empty()) {
            duplicate = !keys.back().insert(parsed.get<std::string>()).second || duplicate;
        }
        return true;
    };
    try {
        Json parsed = Json::parse(line, callback);
        if (!parsed.is_object() || duplicate) {
            return std::nullopt;
        }
        return parsed;
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

bool has_exactly(const Json& object, std::initializer_list<std::string_view> keys) {
    if (object.size() != keys.size()) {
        return false;
    }
    return std::all_of(keys.begin(), keys.end(), [&](std::string_view key) {
        return object.contains(std::string(key));
    });
}

std::optional<std::string> string_field(const Json& object, std::string_view key) {
    const auto found = object.find(std::string(key));
    if (found == object.end() || !found->is_string()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

// A canonical decimal: no sign, no leading zero, within range.
template <typename Integer>
std::optional<Integer> canonical_integer(const std::optional<std::string>& text) {
    if (!text || text->empty() || (text->size() > 1 && text->front() == '0')) {
        return std::nullopt;
    }
    Integer value{};
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size() || value < 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<strategy::PredictedClass> class_named(std::string_view name) {
    for (auto value : {strategy::PredictedClass::Down, strategy::PredictedClass::Flat, strategy::PredictedClass::Up}) {
        if (strategy::predicted_class_name(value) == name) {
            return value;
        }
    }
    return std::nullopt;
}

bool is_lower_hex_64(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

// Inference uses binary64, unlike money. Match Python's exact
// floor(Fraction(binary64_probability) * 1e9); multiplying in double first
// can round upwards across an integer (for example, 0.612).
std::uint64_t confidence_of(double probability) {
    static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53);
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(probability);
    const std::uint64_t exponent = (bits >> 52) & 0x7ff;
    const std::uint64_t mantissa = (bits & ((std::uint64_t{1} << 52) - 1)) |
        (exponent == 0 ? 0 : std::uint64_t{1} << 52);
    const std::int64_t shift = exponent == 0 ? 1074 : 1075 - static_cast<std::int64_t>(exponent);
    if (shift >= 127) {
        return 0;
    }
    const detail::WideInteger scaled = static_cast<detail::WideInteger>(mantissa) * 1'000'000'000;
    return static_cast<std::uint64_t>(scaled >> shift);
}

// A worker error line, for the failure it reports.
std::optional<std::string> error_message(const Json& object) {
    if (string_field(object, "type") != std::optional<std::string>("error")) {
        return std::nullopt;
    }
    return string_field(object, "message").value_or("the worker reported an error");
}

class PipeEnd final {
public:
    PipeEnd() = default;
    explicit PipeEnd(int descriptor) : descriptor_(descriptor) {}
    ~PipeEnd() { reset(); }
    PipeEnd(PipeEnd&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    PipeEnd& operator=(PipeEnd&& other) noexcept {
        if (this != &other) {
            reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }
    PipeEnd(const PipeEnd&) = delete;
    PipeEnd& operator=(const PipeEnd&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    void reset() noexcept {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
    }

private:
    int descriptor_{-1};
};

// The ONNX sandbox containers a registered model's worker starts carry
// its supervisor token (ADR 0021). The worker can be killed at a deadline
// before it removes them, so the server does, by that token.
struct SandboxSupervision {
    std::filesystem::path docker;
    std::string token;
};

constexpr std::string_view kSupervisorLabel = "org.chronos.onnx-sandbox.supervisor";
constexpr auto kDockerTimeout = std::chrono::seconds(15);
// How long after killing a live worker to look again, for a container
// whose creation was still under way.
constexpr auto kSandboxSettle = std::chrono::seconds(1);

std::string new_supervisor_token() {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::random_device random;
    std::string token;
    for (int index = 0; index < 16; ++index) {
        const auto byte = random() & 0xffU;
        token += kHex[byte >> 4];
        token += kHex[byte & 0xfU];
    }
    return token;
}

// Runs the Docker CLI with only PATH, as the worker does, and returns its
// standard output, or nothing if it could not run, failed, or overran.
std::optional<std::string> run_docker(const std::filesystem::path& docker, const std::vector<std::string>& arguments) {
    int output[2] = {-1, -1};
    if (::pipe(output) != 0) {
        return std::nullopt;
    }
    PipeEnd read_end(output[0]);
    PipeEnd write_end(output[1]);
    if (::fcntl(read_end.get(), F_SETFD, FD_CLOEXEC) < 0) {
        return std::nullopt;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    std::vector<std::string> argv_text{docker.string()};
    argv_text.insert(argv_text.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : argv_text) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    std::string path = "PATH=/usr/bin:/bin";
    char* environment[] = {path.data(), nullptr};
    pid_t pid = -1;
    const int spawned = ::posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environment);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        return std::nullopt;
    }
    write_end.reset();
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + kDockerTimeout;
    bool failed = false;
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            failed = true;
            break;
        }
        pollfd descriptor{.fd = read_end.get(), .events = POLLIN, .revents = 0};
        const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            continue;
        }
        char chunk[4096];
        const ssize_t count = ready < 0 ? -1 : ::read(read_end.get(), chunk, sizeof chunk);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            failed = count < 0;
            break;
        }
        text.append(chunk, static_cast<std::size_t>(count));
    }
    int status = 0;
    // EOF only closes the output channel: the CLI may still be running.
    // Waiting for process exit must use the same deadline as reading it.
    while (!failed) {
        const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0
                ? std::optional<std::string>(std::move(text)) : std::nullopt;
        }
        if (reaped < 0 && errno != EINTR) {
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            failed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> supervised_containers(const SandboxSupervision& supervision) {
    const auto listed = run_docker(supervision.docker, {
        "ps", "-a", "--filter", "label=" + std::string(kSupervisorLabel) + "=" + supervision.token, "--format", "{{.Names}}",
    });
    if (!listed) {
        return std::nullopt;
    }
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start < listed->size()) {
        const auto end = std::min(listed->find('\n', start), listed->size());
        if (end > start) {
            names.push_back(listed->substr(start, end - start));
        }
        start = end + 1;
    }
    return names;
}

// Removes every container under the token and checks that Docker then
// lists none.
bool remove_supervised_containers(const SandboxSupervision& supervision, bool settle) {
    bool removed = false;
    for (int look = 0; look < (settle ? 2 : 1); ++look) {
        if (look > 0) {
            std::this_thread::sleep_for(kSandboxSettle);
        }
        const auto names = supervised_containers(supervision);
        if (!names) {
            return false;
        }
        if (!names->empty()) {
            std::vector<std::string> remove{"rm", "-f"};
            remove.insert(remove.end(), names->begin(), names->end());
            (void)run_docker(supervision.docker, remove);
            removed = true;
        }
    }
    if (!removed) {
        return true;
    }
    const auto remaining = supervised_containers(supervision);
    return remaining && remaining->empty();
}

// One worker process: its own process group, its stdin and stdout.
class WorkerProcess final {
public:
    static std::variant<std::unique_ptr<WorkerProcess>, InferenceFailure> spawn(
        const PythonWorkerCommand& command,
        const std::vector<std::string>& arguments,
        std::optional<SandboxSupervision> supervision
    ) {
        int input[2] = {-1, -1};
        int output[2] = {-1, -1};
        if (::pipe(input) != 0) {
            return failure(InferenceFailureCode::SpawnFailed, std::strerror(errno));
        }
        PipeEnd input_read(input[0]);
        PipeEnd input_write(input[1]);
        if (::pipe(output) != 0) {
            return failure(InferenceFailureCode::SpawnFailed, std::strerror(errno));
        }
        PipeEnd output_read(output[0]);
        PipeEnd output_write(output[1]);
        // The server's ends must not leak into later children.
        if (::fcntl(input_write.get(), F_SETFD, FD_CLOEXEC) < 0 ||
            ::fcntl(output_read.get(), F_SETFD, FD_CLOEXEC) < 0 ||
            ::fcntl(input_write.get(), F_SETFL, O_NONBLOCK) < 0) {
            return failure(InferenceFailureCode::SpawnFailed, std::strerror(errno));
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, input_read.get(), STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, output_write.get(), STDOUT_FILENO);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attributes, 0);

        std::vector<std::string> argv_text{command.python.string(), "-m", "chronos_research", "inference-worker"};
        argv_text.insert(argv_text.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& argument : argv_text) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        // Nothing of the server's environment: no credential reaches the
        // worker.
        std::vector<std::string> environment_text{
            "PATH=/usr/bin:/bin",
            "PYTHONPATH=" + command.python_root.string(),
            "PYTHONDONTWRITEBYTECODE=1",
            "PYTHONUNBUFFERED=1",
        };
        std::vector<char*> environment;
        for (auto& entry : environment_text) {
            environment.push_back(entry.data());
        }
        environment.push_back(nullptr);

        pid_t pid = -1;
        const int spawned = ::posix_spawn(&pid, argv[0], &actions, &attributes, argv.data(), environment.data());
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        if (spawned != 0) {
            return failure(InferenceFailureCode::SpawnFailed, std::string("could not start the worker: ") +
                std::strerror(spawned));
        }
        input_read.reset();
        output_write.reset();
        return std::unique_ptr<WorkerProcess>(
            new WorkerProcess(pid, std::move(input_write), std::move(output_read), std::move(supervision))
        );
    }

    ~WorkerProcess() { end(); }

    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;

    // The same deadline bounds sending and receiving: a worker that stops
    // draining stdin must not prevent Stop from joining its runner.
    [[nodiscard]] std::optional<InferenceFailure> write_line(
        const std::string& line,
        std::chrono::steady_clock::time_point deadline
    ) {
        if (line.size() >= kMaximumRequestBytes) {
            return failure(InferenceFailureCode::ProtocolError, "an inference request is too long");
        }
        std::string text = line + "\n";
        std::size_t written = 0;
        while (written < text.size()) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()
            );
            if (remaining.count() <= 0) {
                return failure(InferenceFailureCode::Timeout, "the worker did not accept the request in time");
            }
            const ssize_t count = ::write(input_.get(), text.data() + written, text.size() - written);
            if (count > 0) {
                written += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                return failure(InferenceFailureCode::WorkerExited, "the worker stopped reading");
            }
            pollfd descriptor{.fd = input_.get(), .events = POLLOUT, .revents = 0};
            if (::poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(remaining.count(), 60'000))) < 0 &&
                errno != EINTR) {
                return failure(InferenceFailureCode::WorkerExited, std::strerror(errno));
            }
        }
        return std::nullopt;
    }

    // One line, without its newline, by `deadline`.
    [[nodiscard]] std::variant<std::string, InferenceFailure> read_line(std::chrono::steady_clock::time_point deadline) {
        while (true) {
            const auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                if (newline > kMaximumReplyBytes) {
                    return failure(InferenceFailureCode::ProtocolError, "a worker reply is too long");
                }
                std::string line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                return line;
            }
            if (buffer_.size() > kMaximumReplyBytes) {
                return failure(InferenceFailureCode::ProtocolError, "a worker reply is too long");
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()
            );
            if (remaining.count() <= 0) {
                return failure(InferenceFailureCode::Timeout, "the worker did not reply in time");
            }
            pollfd descriptor{.fd = output_.get(), .events = POLLIN, .revents = 0};
            const int ready = ::poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(remaining.count(), 60'000)));
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return failure(InferenceFailureCode::WorkerExited, std::strerror(errno));
            }
            if (ready == 0) {
                continue;
            }
            char chunk[4096];
            const ssize_t count = ::read(output_.get(), chunk, sizeof chunk);
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                return failure(InferenceFailureCode::WorkerExited, std::strerror(errno));
            }
            if (count == 0) {
                return failure(InferenceFailureCode::WorkerExited, "the worker closed its output");
            }
            buffer_.append(chunk, static_cast<std::size_t>(count));
        }
    }

    // Ends the worker's whole process group and reaps it, then removes the
    // sandbox containers under its token. Closing stdin first lets a
    // healthy worker exit at end of input. The leader is reaped last, so
    // its process group ID cannot be reused while the group is signalled.
    void end() noexcept {
        if (pid_ <= 0) {
            return;
        }
        input_.reset();
        bool exited = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            siginfo_t info{};
            if (::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
                info.si_pid == pid_) {
                exited = true;
                break;
            }
            ::usleep(10'000);
        }
        ::kill(-pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
        output_.reset();
        if (supervision_ && !remove_supervised_containers(*supervision_, !exited)) {
            std::cerr << "chronos: could not confirm that the ONNX sandbox containers of inference worker "
                      << "supervisor " << supervision_->token << " were removed\n";
        }
    }

private:
    WorkerProcess(pid_t pid, PipeEnd input, PipeEnd output, std::optional<SandboxSupervision> supervision)
        : pid_(pid), input_(std::move(input)), output_(std::move(output)), supervision_(std::move(supervision)) {}

    pid_t pid_;
    PipeEnd input_;
    PipeEnd output_;
    std::string buffer_;
    std::optional<SandboxSupervision> supervision_;
};

std::vector<std::string> worker_arguments(
    const PythonWorkerCommand& command,
    const ModelSelection& selection,
    const std::optional<SandboxSupervision>& supervision
) {
    std::vector<std::string> arguments{"--artifacts", command.artifacts.string(), "--instrument", selection.instrument.value()};
    if (const auto* experiment = std::get_if<ExperimentModel>(&selection.model)) {
        arguments.insert(arguments.end(), {"--experiment", experiment->experiment_id, "--model", experiment->name});
        return arguments;
    }
    arguments.insert(arguments.end(), {"--model-version", std::get<RegisteredModel>(selection.model).model_version_id});
    if (supervision) {
        arguments.insert(arguments.end(), {"--docker", supervision->docker.string(), "--sandbox-supervisor", supervision->token});
    }
    return arguments;
}

// Starts a worker and reads its ready line.
std::variant<std::pair<std::unique_ptr<WorkerProcess>, ModelIdentity>, InferenceFailure> start_worker(
    const PythonWorkerCommand& command,
    const ModelSelection& selection,
    bool describe,
    std::chrono::milliseconds timeout
) {
    // Only a registered model may be an import, and only with Docker.
    std::optional<SandboxSupervision> supervision;
    if (command.docker && std::holds_alternative<RegisteredModel>(selection.model)) {
        supervision = SandboxSupervision{.docker = *command.docker, .token = new_supervisor_token()};
    }
    std::vector<std::string> arguments = worker_arguments(command, selection, supervision);
    if (describe) {
        arguments.emplace_back("--describe");
    }
    auto spawned = WorkerProcess::spawn(command, arguments, std::move(supervision));
    if (auto* failed = std::get_if<InferenceFailure>(&spawned)) {
        return std::move(*failed);
    }
    auto worker = std::get<std::unique_ptr<WorkerProcess>>(std::move(spawned));
    auto line = worker->read_line(std::chrono::steady_clock::now() + timeout);
    if (auto* failed = std::get_if<InferenceFailure>(&line)) {
        return std::move(*failed);
    }
    auto identity = parse_ready_line(std::get<std::string>(line), selection);
    if (auto* failed = std::get_if<InferenceFailure>(&identity)) {
        return std::move(*failed);
    }
    return std::pair{std::move(worker), std::get<ModelIdentity>(std::move(identity))};
}

class PythonInferenceSession final : public InferenceSession {
public:
    PythonInferenceSession(std::unique_ptr<WorkerProcess> worker, ModelIdentity identity)
        : worker_(std::move(worker)), identity_(std::move(identity)) {}

    [[nodiscard]] const ModelIdentity& identity() const noexcept override { return identity_; }

    [[nodiscard]] std::variant<InferencePrediction, InferenceFailure> predict(
        const InferenceRequest& request,
        std::chrono::milliseconds timeout
    ) override {
        if (!worker_) {
            return failure(InferenceFailureCode::WorkerExited, "the worker was already ended");
        }
        const auto end_on_failure = [&](InferenceFailure failed) -> std::variant<InferencePrediction, InferenceFailure> {
            if (failed.code != InferenceFailureCode::RequestRefused) {
                worker_.reset();
            }
            return failed;
        };
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (auto failed = worker_->write_line(request_line(request), deadline)) {
            return end_on_failure(std::move(*failed));
        }
        auto line = worker_->read_line(deadline);
        if (auto* failed = std::get_if<InferenceFailure>(&line)) {
            return end_on_failure(std::move(*failed));
        }
        auto prediction = parse_prediction_line(std::get<std::string>(line), request);
        if (auto* failed = std::get_if<InferenceFailure>(&prediction)) {
            return end_on_failure(std::move(*failed));
        }
        return prediction;
    }

private:
    std::unique_ptr<WorkerProcess> worker_;
    ModelIdentity identity_;
};

}  // namespace

std::variant<ModelIdentity, InferenceFailure> parse_ready_line(
    std::string_view line,
    const ModelSelection& selection
) {
    const auto object = parse_object(line);
    if (!object) {
        return failure(InferenceFailureCode::ProtocolError, "the worker's first line is not a JSON object");
    }
    if (const auto message = error_message(*object)) {
        return failure(InferenceFailureCode::ModelRejected, *message);
    }
    const bool registered = std::holds_alternative<RegisteredModel>(selection.model);
    const bool expected_fields = registered
        ? has_exactly(*object, {"cutoff_basis", "family", "instrument", "model_feature_set", "model_version_id",
                                "payload_sha256", "protocol", "training_cutoff_ns", "type"})
        : has_exactly(*object, {"experiment_id", "instrument", "model", "model_feature_set", "parameters_sha256",
                                "protocol", "training_cutoff_ns", "type"});
    if (!expected_fields || string_field(*object, "type") != std::optional<std::string>("ready")) {
        return failure(InferenceFailureCode::ProtocolError, "the worker's ready line has unexpected fields");
    }
    const std::string_view protocol = registered ? kRegisteredInferenceProtocol : kInferenceProtocol;
    if (string_field(*object, "protocol") != std::optional<std::string>(protocol)) {
        return failure(InferenceFailureCode::ProtocolError, "the worker speaks another protocol");
    }
    const Json& feature_set = object->at("model_feature_set");
    if (!feature_set.is_object() || !has_exactly(feature_set, {"name", "version"}) ||
        !string_field(feature_set, "name") || !string_field(feature_set, "version")) {
        return failure(InferenceFailureCode::ProtocolError, "the worker's model feature set is malformed");
    }
    const auto parameters = string_field(*object, registered ? "payload_sha256" : "parameters_sha256");
    const auto cutoff = canonical_integer<std::int64_t>(string_field(*object, "training_cutoff_ns"));
    const auto basis = registered ? string_field(*object, "cutoff_basis") : std::optional<std::string>("verified");
    if (!parameters || !is_lower_hex_64(*parameters) || !cutoff || !basis ||
        (*basis != "verified" && *basis != "declared")) {
        return failure(InferenceFailureCode::ProtocolError, "the worker's model identity is malformed");
    }
    ModelIdentity identity{
        .model = registered
            ? ModelChoice(RegisteredModel{.model_version_id = string_field(*object, "model_version_id").value_or("")})
            : ModelChoice(ExperimentModel{
                  .experiment_id = string_field(*object, "experiment_id").value_or(""),
                  .name = string_field(*object, "model").value_or(""),
              }),
        .instrument = InstrumentId(string_field(*object, "instrument").value_or("unset")),
        .family = string_field(*object, registered ? "family" : "model").value_or(""),
        .parameters_sha256 = *parameters,
        .model_feature_set = *string_field(feature_set, "name") + "/" + *string_field(feature_set, "version"),
        .training_cutoff = Timestamp::from_nanoseconds(*cutoff),
        .cutoff_basis = *basis,
    };
    if (identity.model != selection.model || identity.instrument != selection.instrument || identity.family.empty()) {
        return failure(InferenceFailureCode::ProtocolError, "the worker serves a different model than requested");
    }
    return identity;
}

std::string request_line(const InferenceRequest& request) {
    Json inputs = Json::array();
    for (std::int64_t value : request.inputs) {
        inputs.push_back(std::to_string(value));
    }
    const Json object = {
        {"id", std::to_string(request.id)},
        {"ingest_sequence", std::to_string(request.ingest_sequence)},
        {"inputs", std::move(inputs)},
        {"instrument", request.instrument.value()},
        {"stream", request.stream.value()},
    };
    return object.dump();
}

std::variant<InferencePrediction, InferenceFailure> parse_prediction_line(
    std::string_view line,
    const InferenceRequest& request
) {
    const auto object = parse_object(line);
    if (!object) {
        return failure(InferenceFailureCode::ProtocolError, "a worker reply is not a JSON object");
    }
    if (const auto message = error_message(*object)) {
        // Refused, but answered in order: the worker stays usable.
        if (string_field(*object, "id") == std::optional<std::string>(std::to_string(request.id))) {
            return failure(InferenceFailureCode::RequestRefused, *message);
        }
        return failure(InferenceFailureCode::ProtocolError, "a worker error names another request: " + *message);
    }
    if (!has_exactly(*object, {"class", "confidence_ppb", "id", "ingest_sequence", "instrument", "probabilities",
                               "stream", "type"}) ||
        string_field(*object, "type") != std::optional<std::string>("prediction")) {
        return failure(InferenceFailureCode::ProtocolError, "a prediction has unexpected fields");
    }
    if (string_field(*object, "id") != std::optional<std::string>(std::to_string(request.id)) ||
        string_field(*object, "ingest_sequence") != std::optional<std::string>(std::to_string(request.ingest_sequence)) ||
        string_field(*object, "instrument") != std::optional<std::string>(request.instrument.value()) ||
        string_field(*object, "stream") != std::optional<std::string>(request.stream.value())) {
        return failure(InferenceFailureCode::ProtocolError, "a prediction answers another request");
    }
    const auto predicted = class_named(string_field(*object, "class").value_or(""));
    const auto confidence = canonical_integer<std::uint64_t>(string_field(*object, "confidence_ppb"));
    const Json& probabilities = object->at("probabilities");
    if (!predicted || !confidence || *confidence > 1'000'000'000 || !probabilities.is_array() ||
        probabilities.size() != 3) {
        return failure(InferenceFailureCode::ProtocolError, "a prediction is malformed");
    }
    InferencePrediction prediction{
        .predicted_class = *predicted,
        .probabilities = {},
        .confidence_ppb = *confidence,
    };
    std::array<double, 3> decoded{};
    for (std::size_t index = 0; index < 3; ++index) {
        if (!probabilities[index].is_string()) {
            return failure(InferenceFailureCode::ProtocolError, "a probability is not an encoded number");
        }
        const std::string text = probabilities[index].get<std::string>();
        // Stored in a text[] literal, so only number characters.
        if (text.empty() || text.size() > 32 || !std::all_of(text.begin(), text.end(), [](char character) {
                return (character >= '0' && character <= '9') || character == '.' || character == '-' ||
                    character == '+' || character == 'e' || character == 'E';
            })) {
            return failure(InferenceFailureCode::ProtocolError, "a probability is not an encoded number");
        }
        prediction.probabilities[index] = text;
        // Apple's libc++ does not provide floating-point from_chars. The
        // JSON number parser is portable and locale-independent; only
        // inference probabilities, never financial values, use binary64.
        try {
            const Json number = Json::parse(text);
            if (!number.is_number()) {
                return failure(InferenceFailureCode::ProtocolError, "a probability is not an encoded number");
            }
            decoded[index] = number.get<double>();
        } catch (const Json::exception&) {
            return failure(InferenceFailureCode::ProtocolError, "a probability is not an encoded number");
        }
        if (!std::isfinite(decoded[index]) || decoded[index] < 0 || decoded[index] > 1) {
            return failure(InferenceFailureCode::ProtocolError, "a probability must be finite and between zero and one");
        }
    }
    constexpr double kProbabilitySumTolerance = 1e-12;
    const auto winner = static_cast<std::size_t>(std::max_element(decoded.begin(), decoded.end()) - decoded.begin());
    constexpr std::array classes{strategy::PredictedClass::Down, strategy::PredictedClass::Flat, strategy::PredictedClass::Up};
    if (std::abs(decoded[0] + decoded[1] + decoded[2] - 1) > kProbabilitySumTolerance ||
        classes[winner] != prediction.predicted_class || confidence_of(decoded[winner]) != prediction.confidence_ppb) {
        return failure(InferenceFailureCode::ProtocolError, "the class, probabilities, and confidence disagree");
    }
    return prediction;
}

PythonInferenceProvider::PythonInferenceProvider(PythonWorkerCommand command) : command_(std::move(command)) {}

std::variant<ModelIdentity, InferenceFailure> PythonInferenceProvider::describe(
    const ModelSelection& selection,
    std::chrono::milliseconds timeout
) {
    auto started = start_worker(command_, selection, true, timeout);
    if (auto* failed = std::get_if<InferenceFailure>(&started)) {
        return std::move(*failed);
    }
    return std::get<0>(std::move(started)).second;
}

std::variant<std::unique_ptr<InferenceSession>, InferenceFailure> PythonInferenceProvider::open(
    const ModelSelection& selection,
    std::chrono::milliseconds timeout
) {
    auto started = start_worker(command_, selection, false, timeout);
    if (auto* failed = std::get_if<InferenceFailure>(&started)) {
        return std::move(*failed);
    }
    auto [worker, identity] = std::get<0>(std::move(started));
    return std::unique_ptr<InferenceSession>(
        std::make_unique<PythonInferenceSession>(std::move(worker), std::move(identity))
    );
}

}  // namespace chronos::paper_trading
