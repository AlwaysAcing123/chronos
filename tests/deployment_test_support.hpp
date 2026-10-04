#pragma once

// A scripted inference provider for deployment tests, in place of the
// Python worker: tests set what each prediction answers, inspect how many
// sessions are open, and make opening or describing fail.

#include "chronos/paper_trading/deployment_inference.hpp"
#include "paper_trading_test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace chronos::test {

using paper_trading::InferenceFailure;
using paper_trading::InferencePrediction;
using paper_trading::InferenceProvider;
using paper_trading::InferenceRequest;
using paper_trading::InferenceSession;
using paper_trading::ModelIdentity;
using paper_trading::ModelSelection;
using strategy::PredictedClass;

using Answer = std::function<std::variant<InferencePrediction, InferenceFailure>(const InferenceRequest&)>;

inline InferencePrediction predicted(PredictedClass value, std::uint64_t confidence_ppb = 900'000'000) {
    return InferencePrediction{
        .predicted_class = value,
        .probabilities = {"0.05", "0.05", "0.9"},
        .confidence_ppb = confidence_ppb,
    };
}

struct FakeModel {
    explicit FakeModel(Timestamp cutoff = Timestamp::from_nanoseconds(kMarketOpenNanoseconds + 10'000'000))
        : identity{
              .experiment_id = std::string(64, 'e'),
              .instrument = InstrumentId("AAPL"),
              .model = "logistic",
              .parameters_sha256 = std::string(64, 'f'),
              .model_feature_set = "top_of_book_baseline/1",
              .training_cutoff = cutoff,
          } {}

    ModelIdentity identity;
    // What a new session reports, when it differs from `identity`.
    std::optional<ModelIdentity> served_identity;
    std::optional<InferenceFailure> describe_failure;
    std::optional<InferenceFailure> open_failure;
    std::mutex mutex;
    Answer answer = [](const InferenceRequest&) { return predicted(PredictedClass::Up); };
    std::atomic<int> open_sessions{0};
    std::atomic<int> requests{0};

    void answer_with(Answer next) {
        const std::lock_guard lock(mutex);
        answer = std::move(next);
    }
};

class FakeSession final : public InferenceSession {
public:
    FakeSession(std::shared_ptr<FakeModel> model, ModelIdentity identity)
        : model_(std::move(model)), identity_(std::move(identity)) {
        model_->open_sessions.fetch_add(1);
    }
    ~FakeSession() override { model_->open_sessions.fetch_sub(1); }

    [[nodiscard]] const ModelIdentity& identity() const noexcept override { return identity_; }

    [[nodiscard]] std::variant<InferencePrediction, InferenceFailure> predict(
        const InferenceRequest& request,
        std::chrono::milliseconds
    ) override {
        model_->requests.fetch_add(1);
        Answer answer;
        {
            const std::lock_guard lock(model_->mutex);
            answer = model_->answer;
        }
        return answer(request);
    }

private:
    std::shared_ptr<FakeModel> model_;
    ModelIdentity identity_;
};

class FakeProvider final : public InferenceProvider {
public:
    explicit FakeProvider(std::shared_ptr<FakeModel> model) : model_(std::move(model)) {}

    [[nodiscard]] std::variant<ModelIdentity, InferenceFailure> describe(
        const ModelSelection&,
        std::chrono::milliseconds
    ) override {
        if (model_->describe_failure) {
            return *model_->describe_failure;
        }
        return model_->identity;
    }

    [[nodiscard]] std::variant<std::unique_ptr<InferenceSession>, InferenceFailure> open(
        const ModelSelection&,
        std::chrono::milliseconds
    ) override {
        if (model_->open_failure) {
            return *model_->open_failure;
        }
        return std::unique_ptr<InferenceSession>(
            std::make_unique<FakeSession>(model_, model_->served_identity.value_or(model_->identity))
        );
    }

private:
    std::shared_ptr<FakeModel> model_;
};

}  // namespace chronos::test
