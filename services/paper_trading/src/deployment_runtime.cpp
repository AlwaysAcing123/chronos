#include "chronos/paper_trading/deployment_runtime.hpp"

#include "chronos/sha256.hpp"
#include "chronos/strategy/long_flat_policy.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace chronos::paper_trading {

// ---------------------------------------------------------------------------
// Mailbox and hub

void CandidateMailbox::offer(DeploymentCandidate candidate) {
    {
        const std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        if (slot_) {
            superseded_.fetch_add(1);
        }
        slot_ = std::move(candidate);
        woken_ = true;
    }
    changed_.notify_one();
}

void CandidateMailbox::nudge() {
    {
        const std::lock_guard lock(mutex_);
        woken_ = true;
    }
    changed_.notify_one();
}

void CandidateMailbox::close() {
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
    }
    changed_.notify_all();
}

CandidateMailbox::Taken CandidateMailbox::take(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, timeout, [&] { return woken_ || closed_; });
    Taken taken{.candidate = std::move(slot_), .woken = woken_, .closed = closed_};
    slot_.reset();
    woken_ = false;
    return taken;
}

DeploymentHub::DeploymentHub(market_data::MarketEventObserver* first) : first_(first) {}

void DeploymentHub::observe(const ValidatedMarketEvent& event) {
    // The matcher first: every event is matched against the working orders
    // committed before it, exactly as without deployments.
    if (first_ != nullptr) {
        first_->observe(event);
    }
    const std::optional<features::TopOfBookFeatureRow> row = engine_.apply(event);
    std::vector<std::shared_ptr<CandidateMailbox>> nudged;
    if (const auto* status = std::get_if<MarketStatusEvent>(&event.value())) {
        const std::lock_guard lock(mutex_);
        statuses_[status->header.instrument.value()] = status->status;
        for (const auto& mailbox : mailboxes_) {
            if (mailbox->instrument() == status->header.instrument) {
                nudged.push_back(mailbox);
            }
        }
    }
    for (const auto& mailbox : nudged) {
        mailbox->nudge();
    }
    if (!row) {
        return;
    }
    std::vector<std::shared_ptr<CandidateMailbox>> offered;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& mailbox : mailboxes_) {
            if (mailbox->instrument() == row->instrument && mailbox->stream() == row->stream) {
                offered.push_back(mailbox);
            }
        }
    }
    if (offered.empty()) {
        return;
    }
    const DeploymentCandidate candidate{
        .stream = row->stream,
        .instrument = row->instrument,
        .ingest_sequence = row->ingest_sequence,
        .event_timestamp = row->event_timestamp,
        .receive_timestamp = row->receive_timestamp,
        .inputs = strategy::model_inputs(*row),
    };
    for (const auto& mailbox : offered) {
        mailbox->offer(candidate);
    }
}

bool DeploymentHub::is_trading(const InstrumentId& instrument) const {
    const std::lock_guard lock(mutex_);
    const auto found = statuses_.find(instrument.value());
    // As the feature engine: no status yet means no reason to stop.
    return found == statuses_.end() ||
        (found->second != TradingStatus::Halted && found->second != TradingStatus::Closed);
}

void DeploymentHub::subscribe(const std::shared_ptr<CandidateMailbox>& mailbox) {
    const std::lock_guard lock(mutex_);
    mailboxes_.push_back(mailbox);
}

void DeploymentHub::unsubscribe(const std::shared_ptr<CandidateMailbox>& mailbox) {
    const std::lock_guard lock(mutex_);
    std::erase(mailboxes_, mailbox);
}

// ---------------------------------------------------------------------------
// Runner

namespace {

DeploymentError deployment_error(
    DeploymentErrorCode code,
    std::string reason,
    std::string message,
    std::string field = {}
) {
    return DeploymentError{
        .code = code,
        .field = std::move(field),
        .message = std::move(message),
        .reason = std::move(reason),
    };
}

DeploymentError persistence_error(const std::exception& failure) {
    return deployment_error(
        DeploymentErrorCode::PersistenceUnavailable,
        "persistence_unavailable",
        std::string("PostgreSQL is unavailable: ") + failure.what()
    );
}

std::int64_t elapsed_us(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count();
}

bool is_after(const DeploymentCandidate& candidate, const ModelDeploymentRecord& deployment) {
    if (!deployment.watermark_receive) {
        return true;
    }
    return candidate.receive_timestamp > *deployment.watermark_receive ||
        (candidate.receive_timestamp == *deployment.watermark_receive &&
         candidate.ingest_sequence > *deployment.watermark_ingest_sequence);
}

}  // namespace

class DeploymentManager::Runner final {
public:
    Runner(
        PaperTradingService& service,
        DeploymentHub& hub,
        ModelDeploymentRecord deployment,
        std::unique_ptr<InferenceSession> session,
        const DeploymentRuntimeOptions& options,
        const Logger& log
    )
        : service_(service),
          hub_(hub),
          deployment_(std::move(deployment)),
          session_(std::move(session)),
          options_(options),
          log_(log),
          mailbox_(std::make_shared<CandidateMailbox>(deployment_.config.stream, deployment_.config.instrument)) {
        hub_.subscribe(mailbox_);
        thread_ = std::thread([this] { run(); });
    }

    ~Runner() {
        request_stop();
        join();
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    void request_stop() {
        stop_.store(true);
        mailbox_->close();
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
        hub_.unsubscribe(mailbox_);
        session_.reset();
    }

    [[nodiscard]] bool finished() const noexcept { return finished_.load(); }

    [[nodiscard]] std::optional<std::int64_t> pending() const {
        const std::lock_guard lock(mutex_);
        return pending_ ? std::optional(pending_->decision_sequence) : std::nullopt;
    }

    [[nodiscard]] DeploymentRunCounters counters() const {
        const std::lock_guard lock(mutex_);
        DeploymentRunCounters copy = counters_;
        copy.superseded = mailbox_->superseded();
        return copy;
    }

private:
    void run() {
        try {
            while (!stop_.load() && !exit_) {
                CandidateMailbox::Taken taken = mailbox_->take(options_.idle_wake);
                if (taken.closed || stop_.load()) {
                    break;
                }
                if (has_pending()) {
                    if (taken.candidate) {
                        skip("order_pending");
                    }
                    attempt_submission();
                    continue;
                }
                if (taken.candidate) {
                    consider(*taken.candidate);
                }
            }
        } catch (const std::exception& failure) {
            fault("runtime_error", failure.what());
        }
        finished_.store(true);
    }

    [[nodiscard]] bool has_pending() const {
        const std::lock_guard lock(mutex_);
        return pending_.has_value();
    }

    void skip(const std::string& reason) {
        const std::lock_guard lock(mutex_);
        ++counters_.skipped[reason];
    }

    void note_error(std::string message) {
        if (log_) {
            log_("deployment " + deployment_.config.deployment_id + ": " + message);
        }
        const std::lock_guard lock(mutex_);
        counters_.last_error = std::move(message);
    }

    [[nodiscard]] Timestamp market_now(Timestamp fallback) const {
        return service_.market_state().status().market_time.value_or(fallback);
    }

    void consider(const DeploymentCandidate& candidate) {
        {
            const std::lock_guard lock(mutex_);
            ++counters_.candidates;
        }
        const ModelDeploymentConfig& config = deployment_.config;
        if (candidate.receive_timestamp < config.training_cutoff) {
            return skip("before_training_cutoff");
        }
        if (!is_after(candidate, deployment_)) {
            return skip("not_after_watermark");
        }
        if (deployment_.watermark_receive &&
            candidate.receive_timestamp.nanoseconds() - deployment_.watermark_receive->nanoseconds() <
                config.decision_interval_ns) {
            return skip("decision_interval");
        }
        const auto* inputs = std::get_if<std::array<std::int64_t, strategy::kModelInputs>>(&candidate.inputs);
        if (inputs == nullptr) {
            return skip(
                "inputs_unavailable:" +
                std::string(features::unavailability_name(std::get<features::Unavailability>(candidate.inputs)))
            );
        }
        const Timestamp decided_at = market_now(candidate.receive_timestamp);
        if (decided_at.nanoseconds() - candidate.receive_timestamp.nanoseconds() > config.max_signal_age_ns) {
            return skip("stale_signal");
        }

        const auto started = std::chrono::steady_clock::now();
        auto predicted = session_->predict(
            InferenceRequest{
                .id = ++request_id_,
                .stream = candidate.stream,
                .instrument = candidate.instrument,
                .ingest_sequence = candidate.ingest_sequence,
                .inputs = *inputs,
            },
            options_.inference_timeout
        );
        const std::int64_t took = elapsed_us(started);
        if (const auto* failed = std::get_if<InferenceFailure>(&predicted)) {
            return fault(std::string(inference_failure_name(failed->code)), failed->message);
        }
        const InferencePrediction& prediction = std::get<InferencePrediction>(predicted);
        {
            const std::lock_guard lock(mutex_);
            ++counters_.predictions;
            counters_.last_inference_us = took;
            counters_.max_inference_us = std::max(counters_.max_inference_us, took);
        }

        const PaperAccountResult account = service_.get_account(config.account_id);
        if (const auto* failed = std::get_if<PaperTradingError>(&account)) {
            note_error("the account could not be read: " + failed->message);
            return skip("account_unavailable");
        }
        const Quantity position = std::get<PaperAccountView>(account).state.quantity(config.instrument);
        const strategy::PolicyDecision policy = strategy::decide_long_flat(
            strategy::SchedulePrediction{
                .predicted_class = prediction.predicted_class,
                .confidence_ppb = prediction.confidence_ppb,
            },
            config.threshold_ppb,
            config.target_quantity,
            position,
            false
        );
        const NewDeploymentDecision decision{
            .deployment_id = config.deployment_id,
            .run_generation = deployment_.run_generation,
            .stream = candidate.stream,
            .instrument = candidate.instrument,
            .observation_ingest_sequence = candidate.ingest_sequence,
            .observation_event = candidate.event_timestamp,
            .observation_receive = candidate.receive_timestamp,
            .decided_at_market = decided_at,
            .inputs = *inputs,
            .predicted_class = std::string(strategy::predicted_class_name(prediction.predicted_class)),
            .probabilities = prediction.probabilities,
            .confidence_ppb = prediction.confidence_ppb,
            .position_before = position,
            .target = policy.target,
            .policy_reason = std::string(strategy::policy_reason_name(policy.reason)),
            .side = policy.order ? std::optional(policy.order->side) : std::nullopt,
            .quantity = policy.order ? std::optional(policy.order->quantity) : std::nullopt,
        };
        std::optional<std::variant<DeploymentDecisionRecord, DeploymentConflict>> attempted;
        try {
            attempted = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
                return repository.record_decision(decision);
            });
        } catch (const PaperPersistenceUnavailable& failure) {
            // Whether it committed is unknown: the database says, so the
            // runner adopts its watermark and any decision it holds.
            note_error(std::string("a decision could not be recorded: ") + failure.what());
            skip("persistence_unavailable");
            return resynchronize();
        }
        const auto& recorded = *attempted;
        if (const auto* refused = std::get_if<DeploymentConflict>(&recorded)) {
            if (refused->deployment && refused->deployment->state != DeploymentState::Running) {
                exit_ = true;
                return;
            }
            note_error("a decision was refused: " + refused->message);
            skip("decision_refused");
            return resynchronize();
        }
        const DeploymentDecisionRecord& stored = std::get<DeploymentDecisionRecord>(recorded);
        deployment_.watermark_receive = candidate.receive_timestamp;
        deployment_.watermark_ingest_sequence = candidate.ingest_sequence;
        deployment_.next_decision_sequence = stored.decision_sequence + 1;
        const std::lock_guard lock(mutex_);
        if (stored.state == DecisionState::Pending) {
            ++counters_.order_decisions;
            pending_ = stored;
        } else {
            ++counters_.holds;
        }
    }

    // Reads the deployment back after a decision whose recording is
    // uncertain or was refused: its watermark, and the decision still
    // pending, if any, which this runner then submits or lets expire as if
    // it had recorded it. A failure leaves everything as it was; the next
    // candidate tries again.
    void resynchronize() {
        try {
            const auto current = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
                return repository.find_deployment(deployment_.config.deployment_id);
            });
            if (!current || current->state != DeploymentState::Running ||
                current->run_generation != deployment_.run_generation) {
                exit_ = true;
                return;
            }
            deployment_.watermark_receive = current->watermark_receive;
            deployment_.watermark_ingest_sequence = current->watermark_ingest_sequence;
            deployment_.next_decision_sequence = current->next_decision_sequence;
            if (!current->pending_decision) {
                return;
            }
            const std::int64_t sequence = *current->pending_decision;
            const DecisionPage page = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
                return repository.list_decisions(deployment_.config.deployment_id, 1, sequence + 1);
            });
            if (page.decisions.empty() || page.decisions.front().decision_sequence != sequence) {
                return;
            }
            const std::lock_guard lock(mutex_);
            if (!pending_) {
                ++counters_.order_decisions;
                pending_ = page.decisions.front();
            }
        } catch (const PaperPersistenceUnavailable& failure) {
            note_error(std::string("the deployment could not be read back: ") + failure.what());
        }
    }

    void attempt_submission() {
        DeploymentDecisionRecord pending;
        {
            const std::lock_guard lock(mutex_);
            pending = *pending_;
        }
        const ModelDeploymentConfig& config = deployment_.config;
        // A lock-free look first, so a runner waiting for a later quote does
        // not take the gate and the account for every event. The trusted
        // path decides again under the locks.
        const Timestamp now = market_now(pending.decision.decided_at_market);
        if (now <= *pending.expires_at_market) {
            const ExecutionQuoteResult quote = service_.market_state().execution_quote(config.instrument);
            const auto* current = std::get_if<ExecutionQuote>(&quote);
            if (current == nullptr) {
                return wait("market_data_unavailable");
            }
            if (current->top_of_book.header.ingest_sequence <= pending.decision.observation_ingest_sequence ||
                current->top_of_book.header.receive_timestamp < *pending.not_before_receive) {
                return wait("awaiting_latency");
            }
        }
        const auto submitted = std::chrono::steady_clock::now();
        const DeploymentOrderAttempt attempt = service_.submit_deployment_order(DeploymentOrderRequest{
            .deployment_id = config.deployment_id,
            .run_generation = deployment_.run_generation,
            .decision_sequence = pending.decision_sequence,
        });
        {
            const std::int64_t took = elapsed_us(submitted);
            const std::lock_guard lock(mutex_);
            counters_.last_submission_us = took;
            counters_.max_submission_us = std::max(counters_.max_submission_us, took);
        }
        switch (attempt.outcome) {
            case DeploymentOrderOutcome::Recorded:
                resolved(true);
                return check_limits();
            case DeploymentOrderOutcome::Abandoned:
                resolved(false);
                if (attempt.reason == "signal_expired") {
                    // The deployment decides again on a later candidate.
                    return;
                }
                if (attempt.reason == "account_unhealthy" || attempt.reason == "stream_changed") {
                    return fault(attempt.reason, "the deployment's order can no longer be placed safely");
                }
                // Stopped, faulted, or superseded: no longer this run's.
                exit_ = true;
                return;
            case DeploymentOrderOutcome::NotPending:
                return reconcile();
            case DeploymentOrderOutcome::NotYetEligible:
            case DeploymentOrderOutcome::Busy:
                return wait(attempt.reason);
            case DeploymentOrderOutcome::Unavailable:
                // Uncertain: the decision says whether its order committed.
                note_error("a deployment order's outcome is uncertain: " + attempt.reason);
                return reconcile();
        }
    }

    void wait(const std::string& reason) {
        const std::lock_guard lock(mutex_);
        ++counters_.waits[reason];
    }

    void resolved(bool recorded) {
        const std::lock_guard lock(mutex_);
        pending_.reset();
        if (recorded) {
            ++counters_.orders_recorded;
        } else {
            ++counters_.abandoned;
        }
    }

    // Reads the pending decision back after an uncertain outcome.
    void reconcile() {
        std::int64_t sequence = 0;
        {
            const std::lock_guard lock(mutex_);
            sequence = pending_->decision_sequence;
        }
        try {
            const DecisionPage page = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
                return repository.list_decisions(deployment_.config.deployment_id, 1, sequence + 1);
            });
            if (page.decisions.empty() || page.decisions.front().decision_sequence != sequence) {
                return fault("decision_missing", "a pending decision is no longer recorded");
            }
            switch (page.decisions.front().state) {
                case DecisionState::Pending:
                    // No order exists; the next wake tries again.
                    return;
                case DecisionState::Recorded:
                    resolved(true);
                    return check_limits();
                case DecisionState::Abandoned:
                case DecisionState::Held:
                    resolved(false);
                    exit_ = !still_running();
                    return;
            }
        } catch (const PaperPersistenceUnavailable& failure) {
            note_error(std::string("a pending decision could not be read: ") + failure.what());
        }
    }

    [[nodiscard]] bool still_running() {
        const auto current = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
            return repository.find_deployment(deployment_.config.deployment_id);
        });
        return current && current->state == DeploymentState::Running &&
            current->run_generation == deployment_.run_generation;
    }

    void check_limits() {
        const auto current = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
            return repository.find_deployment(deployment_.config.deployment_id);
        });
        if (!current || current->state != DeploymentState::Running) {
            exit_ = true;
            return;
        }
        if (current->orders_this_run >= deployment_.config.max_orders_per_run) {
            return fault("order_limit_reached", "the run recorded its maximum number of orders");
        }
        if (current->consecutive_rejections >= deployment_.config.max_consecutive_rejections) {
            return fault("rejection_limit_reached", "too many consecutive orders were rejected");
        }
    }

    // Faults the deployment durably, abandons its pending decision, and
    // ends the worker. The runner then exits; positions are kept.
    void fault(const std::string& reason, const std::string& message) {
        note_error(reason + ": " + message);
        exit_ = true;
        try {
            static_cast<void>(service_.with_repository([&](PostgresPaperOrderRepository& repository) {
                return repository.transition_deployment(
                    deployment_.config.deployment_id,
                    {DeploymentState::Running},
                    DeploymentState::Faulted,
                    reason,
                    false
                );
            }));
            if (const auto sequence = pending()) {
                if (service_.abandon_deployment_decision(
                        deployment_.config.deployment_id,
                        *sequence,
                        "faulted",
                        market_now(Timestamp::from_nanoseconds(0))
                    )) {
                    resolved(false);
                }
            }
        } catch (const std::exception& failure) {
            // The runner stops trading regardless; a restart records it as
            // stopped and abandons the decision.
            note_error(std::string("the fault could not be recorded: ") + failure.what());
        }
        session_.reset();
    }

    PaperTradingService& service_;
    DeploymentHub& hub_;
    ModelDeploymentRecord deployment_;
    std::unique_ptr<InferenceSession> session_;
    const DeploymentRuntimeOptions& options_;
    const Logger& log_;
    std::shared_ptr<CandidateMailbox> mailbox_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> finished_{false};
    // Only the runner's thread reads or writes it.
    bool exit_{false};
    std::uint64_t request_id_{0};
    mutable std::mutex mutex_;
    std::optional<DeploymentDecisionRecord> pending_;
    DeploymentRunCounters counters_;
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// Manager

DeploymentManager::DeploymentManager(
    PaperTradingService& service,
    DeploymentHub& hub,
    std::unique_ptr<InferenceProvider> provider,
    DeploymentRuntimeOptions options,
    Logger log
)
    : service_(service),
      hub_(hub),
      provider_(std::move(provider)),
      options_(std::move(options)),
      log_(std::move(log)) {}

DeploymentManager::~DeploymentManager() { shutdown(); }

void DeploymentManager::recover() {
    const std::lock_guard control(control_mutex_);
    std::optional<std::int64_t> before;
    std::vector<ModelDeploymentRecord> deployments;
    do {
        DeploymentPage page = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
            return repository.list_deployments(200, before);
        });
        before = page.next_before_sequence;
        for (auto& deployment : page.deployments) {
            deployments.push_back(std::move(deployment));
        }
    } while (before);
    for (const auto& deployment : deployments) {
        if (deployment.state == DeploymentState::Running || deployment.state == DeploymentState::Stopping) {
            static_cast<void>(service_.with_repository([&](PostgresPaperOrderRepository& repository) {
                return repository.transition_deployment(
                    deployment.config.deployment_id,
                    {DeploymentState::Running, DeploymentState::Stopping},
                    DeploymentState::Stopped,
                    "server_restarted",
                    false
                );
            }));
            if (log_) {
                log_("deployment " + deployment.config.deployment_id + " was running; now stopped (server_restarted)");
            }
        }
    }
    const auto pending = service_.with_repository([](PostgresPaperOrderRepository& repository) {
        return repository.pending_decisions();
    });
    for (const auto& decision : pending) {
        static_cast<void>(service_.abandon_deployment_decision(
            decision.decision.deployment_id,
            decision.decision_sequence,
            "server_restarted",
            decision.decision.decided_at_market
        ));
    }
}

std::string DeploymentManager::canonical_request(const DeploymentCreateRequest& request) {
    // One field per line, in a fixed order, each value in its canonical
    // text; no field can contain a newline.
    return std::string("chronos.model_deployment_request/1\n") +
        "deployment_id=" + request.deployment_id + "\n" +
        "account_id=" + request.account_id + "\n" +
        "starting_cash=" + request.starting_cash.to_string() + "\n" +
        "experiment_id=" + request.experiment_id + "\n" +
        "model=" + request.model + "\n" +
        "instrument=" + request.instrument + "\n" +
        "policy=" + std::string(kDeploymentPolicy) + "\n" +
        "threshold_ppb=" + std::to_string(request.threshold_ppb) + "\n" +
        "target_quantity=" + request.target_quantity.to_string() + "\n" +
        "decision_interval_ns=" + std::to_string(request.decision_interval_ns) + "\n" +
        "latency_ns=" + std::to_string(request.latency_ns) + "\n" +
        "max_signal_age_ns=" + std::to_string(request.max_signal_age_ns) + "\n" +
        "max_orders_per_run=" + std::to_string(request.max_orders_per_run) + "\n" +
        "max_consecutive_rejections=" + std::to_string(request.max_consecutive_rejections) + "\n";
}

std::optional<DeploymentError> DeploymentManager::validate(const DeploymentCreateRequest& request) const {
    const auto invalid = [](std::string field, std::string message) {
        return deployment_error(DeploymentErrorCode::InvalidRequest, "invalid_request", std::move(message), std::move(field));
    };
    // The schema's rule for deployment IDs: shorter than an account ID,
    // so `<deployment>-<12 digits>` is a valid client order ID.
    const auto& id = request.deployment_id;
    if (id.empty() || id.size() > 32 || !is_valid_identifier(id)) {
        return invalid("deployment_id", "deployment_id must be 1 to 32 characters from [A-Za-z0-9._-] starting with a letter or digit");
    }
    if (id == "kill-switch") {
        return invalid("deployment_id", "kill-switch is reserved for the deployment control route");
    }
    if (!is_valid_identifier(request.account_id)) {
        return invalid("account_id", "account_id must be 1 to 64 characters from [A-Za-z0-9._-] starting with a letter or digit");
    }
    if (request.starting_cash.raw() <= 0) {
        return invalid("starting_cash", "starting cash must be positive");
    }
    if (!is_sha256_hex(request.experiment_id)) {
        return invalid("experiment_id", "experiment_id must be 64 lowercase hexadecimal digits");
    }
    if (request.model != "logistic" && request.model != "majority") {
        return invalid("model", "model must be logistic or majority");
    }
    if (std::find(options_.tradeable_instruments.begin(), options_.tradeable_instruments.end(),
                  InstrumentId(request.instrument)) == options_.tradeable_instruments.end()) {
        return invalid("instrument", "instrument is not tradeable on this server");
    }
    if (request.threshold_ppb > 1'000'000'000) {
        return invalid("threshold", "threshold must be between 0 and 1");
    }
    if (request.target_quantity.raw() <= 0 || request.target_quantity > Quantity::parse("1")) {
        return invalid("target_quantity", "target_quantity must be positive and at most one share");
    }
    if (request.decision_interval_ns <= 0) {
        return invalid("decision_interval_ms", "decision_interval_ms must be positive");
    }
    if (request.latency_ns <= 0) {
        return invalid("latency_ms", "latency_ms must be positive");
    }
    if (request.max_signal_age_ns <= request.latency_ns) {
        return invalid("max_signal_age_ms", "max_signal_age_ms must exceed latency_ms");
    }
    if (request.max_orders_per_run <= 0 || request.max_orders_per_run > 10'000) {
        return invalid("max_orders_per_run", "max_orders_per_run must be between 1 and 10000");
    }
    if (request.max_consecutive_rejections <= 0 || request.max_consecutive_rejections > 1'000) {
        return invalid("max_consecutive_rejections", "max_consecutive_rejections must be between 1 and 1000");
    }
    return std::nullopt;
}

namespace {

DeploymentError disabled_error() {
    return deployment_error(
        DeploymentErrorCode::Disabled,
        "deployments_disabled",
        "model deployments are disabled; start the server with --model-deployments"
    );
}

}  // namespace

DeploymentCreateResult DeploymentManager::create(const DeploymentCreateRequest& request) {
    if (!enabled()) {
        return disabled_error();
    }
    if (auto invalid = validate(request)) {
        return std::move(*invalid);
    }
    const std::lock_guard control(control_mutex_);
    const ModelSelection selection{
        .experiment_id = request.experiment_id,
        .instrument = InstrumentId(request.instrument),
        .model = request.model,
    };
    auto described = provider_->describe(selection, options_.model_load_timeout);
    if (const auto* failed = std::get_if<InferenceFailure>(&described)) {
        return deployment_error(
            DeploymentErrorCode::ModelUnavailable,
            std::string(inference_failure_name(failed->code)),
            "the model could not be verified: " + failed->message,
            "experiment_id"
        );
    }
    const ModelIdentity& identity = std::get<ModelIdentity>(described);
    if (identity.model_feature_set != kDeploymentModelFeatureSet) {
        return deployment_error(
            DeploymentErrorCode::ModelUnavailable,
            "unsupported_feature_set",
            "the model uses feature set " + identity.model_feature_set
        );
    }
    PreTradeRiskLimits limits = options_.account_risk_limits;
    // Server-side limits: the account can never hold or order more than
    // the deployment's target, whatever the policy computes.
    limits.maximum_order_quantity = std::min(limits.maximum_order_quantity, request.target_quantity);
    limits.maximum_position_quantity = std::min(limits.maximum_position_quantity, request.target_quantity);
    ModelDeploymentConfig config;
    config.deployment_id = request.deployment_id;
    config.account_id = request.account_id;
    config.starting_cash = request.starting_cash;
    config.experiment_id = identity.experiment_id;
    config.parameters_sha256 = identity.parameters_sha256;
    config.model_name = identity.model;
    config.instrument = identity.instrument;
    config.training_cutoff = identity.training_cutoff;
    config.threshold_ppb = request.threshold_ppb;
    config.target_quantity = request.target_quantity;
    config.decision_interval_ns = request.decision_interval_ns;
    config.latency_ns = request.latency_ns;
    config.max_signal_age_ns = request.max_signal_age_ns;
    config.max_orders_per_run = request.max_orders_per_run;
    config.max_consecutive_rejections = request.max_consecutive_rejections;
    config.stream = options_.stream;
    config.risk_limits = limits;
    config.request_sha256 = Sha256::hex(canonical_request(request));
    try {
        auto created = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
            return repository.create_deployment(config);
        });
        if (const auto* conflict = std::get_if<DeploymentConflict>(&created)) {
            return deployment_error(
                DeploymentErrorCode::Conflict,
                conflict->code == DeploymentConflictCode::AccountExists ? "account_exists" : "deployment_exists",
                conflict->message,
                conflict->code == DeploymentConflictCode::AccountExists ? "account_id" : "deployment_id"
            );
        }
        auto& made = std::get<CreatedDeployment>(created);
        return DeploymentCreated{.status = status_of(std::move(made.deployment)), .created = made.created};
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_error(failure);
    }
}

DeploymentStatus DeploymentManager::status_of(ModelDeploymentRecord record) {
    const std::lock_guard lock(runners_mutex_);
    DeploymentStatus status{.deployment = std::move(record), .active = false, .run = std::nullopt};
    const auto runner = runners_.find(status.deployment.config.deployment_id);
    if (runner != runners_.end()) {
        status.active = !runner->second->finished();
        status.run = runner->second->counters();
    } else if (const auto run = finished_runs_.find(status.deployment.config.deployment_id); run != finished_runs_.end()) {
        status.run = run->second;
    }
    return status;
}

DeploymentResult DeploymentManager::get(std::string_view deployment_id) {
    try {
        auto found = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
            return repository.find_deployment(deployment_id);
        });
        if (!found) {
            return deployment_error(DeploymentErrorCode::DeploymentNotFound, "deployment_not_found", "no such deployment");
        }
        return status_of(std::move(*found));
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_error(failure);
    }
}

DeploymentListResult DeploymentManager::list(std::size_t limit, std::optional<std::int64_t> before_sequence) {
    try {
        DeploymentPage page = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
            return repository.list_deployments(limit, before_sequence);
        });
        DeploymentListPage listed{.deployments = {}, .next_before_sequence = page.next_before_sequence};
        for (auto& deployment : page.deployments) {
            listed.deployments.push_back(status_of(std::move(deployment)));
        }
        return listed;
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_error(failure);
    }
}

DecisionListResult DeploymentManager::decisions(
    std::string_view deployment_id,
    std::size_t limit,
    std::optional<std::int64_t> before_sequence
) {
    try {
        return service_.with_repository([&](PostgresPaperOrderRepository& repository) -> DecisionListResult {
            if (!repository.find_deployment(deployment_id)) {
                return deployment_error(DeploymentErrorCode::DeploymentNotFound, "deployment_not_found", "no such deployment");
            }
            return repository.list_decisions(deployment_id, limit, before_sequence);
        });
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_error(failure);
    }
}

void DeploymentManager::reap_finished_locked() {
    std::vector<std::unique_ptr<Runner>> finished;
    {
        const std::lock_guard lock(runners_mutex_);
        for (auto entry = runners_.begin(); entry != runners_.end();) {
            if (entry->second->finished()) {
                finished_runs_[entry->first] = entry->second->counters();
                finished.push_back(std::move(entry->second));
                entry = runners_.erase(entry);
            } else {
                ++entry;
            }
        }
    }
    // Joined outside the readers' lock.
    finished.clear();
}

DeploymentResult DeploymentManager::start(std::string_view deployment_id) {
    if (!enabled()) {
        return disabled_error();
    }
    const std::lock_guard control(control_mutex_);
    reap_finished_locked();
    try {
        auto found = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
            return repository.find_deployment(deployment_id);
        });
        if (!found) {
            return deployment_error(DeploymentErrorCode::DeploymentNotFound, "deployment_not_found", "no such deployment");
        }
        bool active = false;
        {
            const std::lock_guard lock(runners_mutex_);
            active = runners_.contains(std::string(deployment_id));
        }
        if (active) {
            // Already running in this process: a repeated start.
            return status_of(std::move(*found));
        }
        {
            const std::lock_guard lock(runners_mutex_);
            if (runners_.size() >= options_.maximum_active_deployments) {
                return deployment_error(
                    DeploymentErrorCode::InvalidState,
                    "deployment_capacity_reached",
                    "the server is at its active deployment limit; stop one before starting another"
                );
            }
        }
        if (found->state == DeploymentState::Running || found->state == DeploymentState::Stopping) {
            return deployment_error(
                DeploymentErrorCode::InvalidState,
                "invalid_state",
                "the deployment is " + std::string(deployment_state_name(found->state)) + " without a runner; stop it first"
            );
        }
        const ModelDeploymentConfig& config = found->config;
        if (config.stream != options_.stream) {
            return deployment_error(
                DeploymentErrorCode::InvalidState,
                "stream_mismatch",
                "the deployment trades stream " + config.stream.value() + ", not this server's " + options_.stream.value()
            );
        }
        if (found->pending_decision) {
            static_cast<void>(service_.abandon_deployment_decision(
                deployment_id, *found->pending_decision, "stopped", service_.market_state().status().market_time.value_or(Timestamp::from_nanoseconds(0))
            ));
        }
        const auto fault_with = [&](std::string reason, std::string message) -> DeploymentResult {
            static_cast<void>(service_.with_repository([&](PostgresPaperOrderRepository& repository) {
                return repository.transition_deployment(
                    deployment_id, {DeploymentState::Stopped, DeploymentState::Faulted}, DeploymentState::Faulted, reason, false
                );
            }));
            return deployment_error(DeploymentErrorCode::ModelUnavailable, std::move(reason), std::move(message));
        };
        auto opened = provider_->open(
            ModelSelection{.experiment_id = config.experiment_id, .instrument = config.instrument, .model = config.model_name},
            options_.model_load_timeout
        );
        if (const auto* failed = std::get_if<InferenceFailure>(&opened)) {
            return fault_with(std::string(inference_failure_name(failed->code)), "the model could not be loaded: " + failed->message);
        }
        auto session = std::get<std::unique_ptr<InferenceSession>>(std::move(opened));
        const ModelIdentity expected{
            .experiment_id = config.experiment_id,
            .instrument = config.instrument,
            .model = config.model_name,
            .parameters_sha256 = config.parameters_sha256,
            .model_feature_set = std::string(kDeploymentModelFeatureSet),
            .training_cutoff = config.training_cutoff,
        };
        if (session->identity() != expected) {
            session.reset();
            return fault_with(
                "model_identity_mismatch",
                "the model's verified identity differs from the one the deployment was created with"
            );
        }
        auto started = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
            return repository.transition_deployment(
                deployment_id, {DeploymentState::Stopped, DeploymentState::Faulted}, DeploymentState::Running, "started", true
            );
        });
        if (const auto* conflict = std::get_if<DeploymentConflict>(&started)) {
            return deployment_error(DeploymentErrorCode::InvalidState, "invalid_state", conflict->message);
        }
        ModelDeploymentRecord running = std::get<ModelDeploymentRecord>(std::move(started));
        auto runner = std::make_unique<Runner>(service_, hub_, running, std::move(session), options_, log_);
        {
            const std::lock_guard lock(runners_mutex_);
            finished_runs_.erase(std::string(deployment_id));
            runners_.emplace(std::string(deployment_id), std::move(runner));
        }
        if (log_) {
            log_("deployment " + std::string(deployment_id) + " started (run " + std::to_string(running.run_generation) + ")");
        }
        return status_of(std::move(running));
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_error(failure);
    }
}

DeploymentResult DeploymentManager::stop(std::string_view deployment_id, std::string_view reason) {
    const std::lock_guard control(control_mutex_);
    reap_finished_locked();
    return stop_locked(deployment_id, reason);
}

DeploymentResult DeploymentManager::stop_locked(std::string_view deployment_id, std::string_view reason) {
    std::unique_ptr<Runner> runner;
    {
        const std::lock_guard lock(runners_mutex_);
        const auto found = runners_.find(std::string(deployment_id));
        if (found != runners_.end()) {
            runner = std::move(found->second);
            runners_.erase(found);
        }
    }
    // The runner stops trading first, whatever PostgreSQL can record.
    if (runner) {
        runner->request_stop();
    }
    try {
        static_cast<void>(service_.with_repository([&](PostgresPaperOrderRepository& repository) {
            return repository.transition_deployment(
                deployment_id, {DeploymentState::Running}, DeploymentState::Stopping, reason, false
            );
        }));
    } catch (const PaperPersistenceUnavailable&) {
        // Recorded below if PostgreSQL returns; the runner stops either way.
    }
    std::optional<std::int64_t> pending;
    if (runner) {
        runner->join();
        pending = runner->pending();
        const std::lock_guard lock(runners_mutex_);
        finished_runs_[std::string(deployment_id)] = runner->counters();
    }
    runner.reset();
    try {
        auto found = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
            return repository.find_deployment(deployment_id);
        });
        if (!found) {
            return deployment_error(DeploymentErrorCode::DeploymentNotFound, "deployment_not_found", "no such deployment");
        }
        if (!pending) {
            pending = found->pending_decision;
        }
        if (pending) {
            static_cast<void>(service_.abandon_deployment_decision(
                deployment_id, *pending, reason, service_.market_state().status().market_time.value_or(Timestamp::from_nanoseconds(0))
            ));
        }
        if (found->state == DeploymentState::Running || found->state == DeploymentState::Stopping) {
            auto stopped = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
                return repository.transition_deployment(
                    deployment_id, {DeploymentState::Running, DeploymentState::Stopping}, DeploymentState::Stopped, reason, false
                );
            });
            if (auto* record = std::get_if<ModelDeploymentRecord>(&stopped)) {
                if (log_) {
                    log_("deployment " + std::string(deployment_id) + " stopped (" + std::string(reason) + ")");
                }
                return status_of(std::move(*record));
            }
        }
        auto current = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
            return repository.find_deployment(deployment_id);
        });
        return status_of(std::move(*current));
    } catch (const PaperPersistenceUnavailable& failure) {
        DeploymentError error = persistence_error(failure);
        error.message = "the deployment stopped trading, but its stopped state could not be recorded (" +
            std::string(failure.what()) + "); a restart records it as stopped";
        return error;
    }
}

std::variant<std::vector<DeploymentStatus>, DeploymentError> DeploymentManager::kill_switch() {
    const std::lock_guard control(control_mutex_);
    reap_finished_locked();
    // Every runner stops trading before anything is read or written.
    {
        const std::lock_guard lock(runners_mutex_);
        for (auto& [id, runner] : runners_) {
            runner->request_stop();
        }
    }
    std::vector<std::string> ids;
    {
        const std::lock_guard lock(runners_mutex_);
        for (const auto& [id, runner] : runners_) {
            ids.push_back(id);
        }
    }
    try {
        std::optional<std::int64_t> before;
        do {
            DeploymentPage page = service_.with_repository([&](PostgresPaperOrderRepository& repository) {
                return repository.list_deployments(200, before);
            });
            before = page.next_before_sequence;
            for (const auto& deployment : page.deployments) {
                if ((deployment.state == DeploymentState::Running || deployment.state == DeploymentState::Stopping ||
                     deployment.pending_decision) &&
                    std::find(ids.begin(), ids.end(), deployment.config.deployment_id) == ids.end()) {
                    ids.push_back(deployment.config.deployment_id);
                }
            }
        } while (before);
    } catch (const PaperPersistenceUnavailable& failure) {
        // Runners have stopped; stop them fully and report.
        for (const auto& id : ids) {
            static_cast<void>(stop_locked(id, "kill_switch"));
        }
        return persistence_error(failure);
    }
    std::vector<DeploymentStatus> stopped;
    for (const auto& id : ids) {
        auto result = stop_locked(id, "kill_switch");
        if (auto* status = std::get_if<DeploymentStatus>(&result)) {
            stopped.push_back(std::move(*status));
        } else {
            return std::get<DeploymentError>(std::move(result));
        }
    }
    if (log_) {
        log_("KILL SWITCH: " + std::to_string(stopped.size()) + " deployment(s) stopped; positions kept");
    }
    return stopped;
}

void DeploymentManager::shutdown() {
    const std::lock_guard control(control_mutex_);
    std::vector<std::string> ids;
    {
        const std::lock_guard lock(runners_mutex_);
        for (const auto& [id, runner] : runners_) {
            ids.push_back(id);
        }
    }
    for (const auto& id : ids) {
        try {
            static_cast<void>(stop_locked(id, "server_stopped"));
        } catch (const std::exception& failure) {
            if (log_) {
                log_("deployment " + id + " could not be stopped cleanly: " + failure.what());
            }
        }
    }
}

}  // namespace chronos::paper_trading
