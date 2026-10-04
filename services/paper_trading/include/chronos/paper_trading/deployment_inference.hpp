#pragma once

#include "chronos/market_data_types.hpp"
#include "chronos/strategy/inference_schedule.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>

// Model inference for deployments (ADR 0018): a verified research model,
// served by a long-lived worker process speaking chronos.inference_worker/1
// (python/chronos_research/inference_worker.py).
namespace chronos::paper_trading {

inline constexpr std::string_view kInferenceProtocol = "chronos.inference_worker/1";

// Which model to serve.
struct ModelSelection {
    std::string experiment_id;
    InstrumentId instrument{"unset"};
    // "logistic" or "majority".
    std::string model;
};

// The model as the worker verified it, before serving anything.
struct ModelIdentity {
    std::string experiment_id;
    InstrumentId instrument{"unset"};
    std::string model;
    std::string parameters_sha256;
    // "top_of_book_baseline/1".
    std::string model_feature_set;
    // No decision on an observation received before this.
    Timestamp training_cutoff;

    friend bool operator==(const ModelIdentity&, const ModelIdentity&) = default;
};

struct InferenceRequest {
    std::uint64_t id{0};
    MarketDataStreamId stream{"unset"};
    InstrumentId instrument{"unset"};
    std::uint64_t ingest_sequence{0};
    std::array<std::int64_t, strategy::kModelInputs> inputs{};
};

struct InferencePrediction {
    strategy::PredictedClass predicted_class{strategy::PredictedClass::Flat};
    // Exactly as the worker encoded them.
    std::array<std::string, 3> probabilities;
    std::uint64_t confidence_ppb{0};
};

enum class InferenceFailureCode {
    // The worker could not be started.
    SpawnFailed,
    // The worker refused the model: unknown, unevaluable, or not verified.
    ModelRejected,
    // No complete reply in time. The worker is ended.
    Timeout,
    // The worker ended, or its pipes closed.
    WorkerExited,
    // A reply that is not exactly the protocol's. The worker is ended.
    ProtocolError,
    // The worker answered this request with an error line.
    RequestRefused,
};

[[nodiscard]] std::string_view inference_failure_name(InferenceFailureCode code) noexcept;

struct InferenceFailure {
    InferenceFailureCode code;
    std::string message;
};

// One served model. Not thread-safe: one runner owns it.
class InferenceSession {
public:
    virtual ~InferenceSession() = default;

    [[nodiscard]] virtual const ModelIdentity& identity() const noexcept = 0;

    // After any failure but RequestRefused the session is unusable.
    [[nodiscard]] virtual std::variant<InferencePrediction, InferenceFailure> predict(
        const InferenceRequest& request,
        std::chrono::milliseconds timeout
    ) = 0;

protected:
    InferenceSession() = default;
    InferenceSession(const InferenceSession&) = default;
    InferenceSession& operator=(const InferenceSession&) = default;
};

class InferenceProvider {
public:
    virtual ~InferenceProvider() = default;

    // Verifies the model and reports its identity without serving it.
    [[nodiscard]] virtual std::variant<ModelIdentity, InferenceFailure> describe(
        const ModelSelection& selection,
        std::chrono::milliseconds timeout
    ) = 0;

    [[nodiscard]] virtual std::variant<std::unique_ptr<InferenceSession>, InferenceFailure> open(
        const ModelSelection& selection,
        std::chrono::milliseconds timeout
    ) = 0;

protected:
    InferenceProvider() = default;
    InferenceProvider(const InferenceProvider&) = default;
    InferenceProvider& operator=(const InferenceProvider&) = default;
};

// Runs `python -m chronos_research inference-worker` from `python_root`
// (the repository's python directory) against `artifacts` (the directory
// holding experiments/). The worker gets its own process group and a
// minimal environment: PATH, PYTHONPATH, and nothing of the server's, so no
// credential reaches it. Its stderr is the server's.
//
// Writing to a worker that has exited raises SIGPIPE; the server ignores
// SIGPIPE whenever deployments are enabled.
struct PythonWorkerCommand {
    std::filesystem::path python;
    std::filesystem::path python_root;
    std::filesystem::path artifacts;
};

class PythonInferenceProvider final : public InferenceProvider {
public:
    explicit PythonInferenceProvider(PythonWorkerCommand command);

    [[nodiscard]] std::variant<ModelIdentity, InferenceFailure> describe(
        const ModelSelection& selection,
        std::chrono::milliseconds timeout
    ) override;

    [[nodiscard]] std::variant<std::unique_ptr<InferenceSession>, InferenceFailure> open(
        const ModelSelection& selection,
        std::chrono::milliseconds timeout
    ) override;

private:
    PythonWorkerCommand command_;
};

// The worker's ready line, parsed strictly. Exposed for tests.
[[nodiscard]] std::variant<ModelIdentity, InferenceFailure> parse_ready_line(
    std::string_view line,
    const ModelSelection& selection
);

// A prediction line, parsed strictly and matched against its request.
[[nodiscard]] std::variant<InferencePrediction, InferenceFailure> parse_prediction_line(
    std::string_view line,
    const InferenceRequest& request
);

[[nodiscard]] std::string request_line(const InferenceRequest& request);

}  // namespace chronos::paper_trading
