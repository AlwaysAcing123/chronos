#pragma once

#include "chronos/features/top_of_book_features.hpp"
#include "chronos/market_data/market_data_source.hpp"
#include "chronos/market_data/market_event_observer.hpp"
#include "chronos/paper_trading/deployment_inference.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"
#include "chronos/strategy/inference_schedule.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

// The live half of model deployments (ADR 0018): from each market event to
// point-in-time features, a bounded hand-off to each running deployment, a
// prediction, the long/flat policy, a durable decision, and the trusted
// order path.
//
// Lock order. The feed holds the market-event gate exclusively while it
// calls DeploymentHub::observe, so observe takes only leaf mutexes (the
// hub's and each mailbox's) and never waits for anything else. A runner
// holds no mutex of its own while it calls the service, which takes
// submission_mutex_, the gate (shared), its connection mutex, then rows.
// The manager's control mutex is taken only by control requests and never
// by a runner, so stopping a runner can wait for it to finish.
namespace chronos::paper_trading {

// What a deployment may decide on: one top-of-book row of its instrument
// and stream, with the model inputs computed from it.
struct DeploymentCandidate {
    MarketDataStreamId stream{"unset"};
    InstrumentId instrument{"unset"};
    std::uint64_t ingest_sequence{0};
    Timestamp event_timestamp;
    Timestamp receive_timestamp;
    strategy::ModelInputs inputs{features::Unavailability::MissingBothSides};
};

// The bounded hand-off from the feed to one runner: one slot holding the
// latest candidate. Offering never waits; a candidate the runner had not
// taken yet is replaced and counted as superseded.
class CandidateMailbox final {
public:
    CandidateMailbox(MarketDataStreamId stream, InstrumentId instrument)
        : stream_(std::move(stream)), instrument_(std::move(instrument)) {}

    [[nodiscard]] const MarketDataStreamId& stream() const noexcept { return stream_; }
    [[nodiscard]] const InstrumentId& instrument() const noexcept { return instrument_; }

    void offer(DeploymentCandidate candidate);
    // Wakes the runner without a candidate: the instrument's trading status
    // changed.
    void nudge();
    void close();

    struct Taken {
        std::optional<DeploymentCandidate> candidate;
        // Something arrived since the last take, candidate or not.
        bool woken{false};
        bool closed{false};
    };
    // Waits up to `timeout` for anything.
    [[nodiscard]] Taken take(std::chrono::milliseconds timeout);

    [[nodiscard]] std::uint64_t superseded() const noexcept { return superseded_.load(); }

private:
    const MarketDataStreamId stream_;
    const InstrumentId instrument_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<DeploymentCandidate> slot_;
    bool woken_{false};
    bool closed_{false};
    std::atomic<std::uint64_t> superseded_{0};
};

// The feed's one observer: the working-order matcher first, so every event
// is matched before anything else sees it, then the feature engine, then
// each subscribed mailbox. Also the service's view of trading status,
// updated under the same exclusive gate the service reads it under.
class DeploymentHub final : public market_data::MarketEventObserver, public TradingStatusView {
public:
    // `first` may be null; it must outlive the hub.
    explicit DeploymentHub(market_data::MarketEventObserver* first);

    void observe(const ValidatedMarketEvent& event) override;

    [[nodiscard]] bool is_trading(const InstrumentId& instrument) const override;
    // The instrument's latest trading status, or nullopt before any.
    [[nodiscard]] std::optional<TradingStatus> trading_status(const InstrumentId& instrument) const;

    void subscribe(const std::shared_ptr<CandidateMailbox>& mailbox);
    void unsubscribe(const std::shared_ptr<CandidateMailbox>& mailbox);

private:
    market_data::MarketEventObserver* first_;
    // Used only on the feed's thread.
    features::TopOfBookFeatureEngine engine_;
    mutable std::mutex mutex_;
    std::map<std::string, TradingStatus> statuses_;
    std::vector<std::shared_ptr<CandidateMailbox>> mailboxes_;
};

struct DeploymentRuntimeOptions {
    // The server's one stream and its tradeable instruments.
    MarketDataStreamId stream{"unset"};
    std::vector<InstrumentId> tradeable_instruments;
    // The server's account limits; a deployment's account also limits
    // order and position quantity to the deployment's target.
    PreTradeRiskLimits account_risk_limits;
    // Bounds the aggregate worker/thread/mailbox cost, not just each worker.
    std::size_t maximum_active_deployments{8};
    std::chrono::milliseconds model_load_timeout{30'000};
    std::chrono::milliseconds inference_timeout{2'000};
    // How often an idle runner checks the market clock, so a pending
    // decision expires on time even without events.
    std::chrono::milliseconds idle_wake{200};
    // The server's market-data source, read when a status is rendered.
    // Without it, a status says nothing about the source.
    std::function<market_data::MarketDataSourceStatus()> source_status{};
};

// A create request, as validated input.
struct DeploymentCreateRequest {
    std::string deployment_id;
    std::string account_id;
    Money starting_cash;
    // An experiment's model, or a registered model version.
    ModelChoice model;
    std::string instrument;
    std::uint64_t threshold_ppb{0};
    Quantity target_quantity;
    std::int64_t decision_interval_ns{0};
    std::int64_t latency_ns{0};
    std::int64_t max_signal_age_ns{0};
    std::int32_t max_orders_per_run{0};
    std::int32_t max_consecutive_rejections{0};
};

enum class DeploymentErrorCode {
    InvalidRequest,
    DeploymentNotFound,
    // Created from a different request, or the account ID is taken.
    Conflict,
    // The state does not allow it, such as starting a stopping deployment.
    InvalidState,
    // The model could not be verified, or differs from the deployment's.
    ModelUnavailable,
    PersistenceUnavailable,
    // The server was not started with model deployments enabled.
    Disabled,
};

struct DeploymentError {
    DeploymentErrorCode code;
    std::string field;
    std::string message;
    // A stable machine-readable reason, such as model_identity_mismatch.
    std::string reason;
};

// What a run has done since it started, kept in memory only.
struct DeploymentRunCounters {
    std::uint64_t candidates{0};
    std::uint64_t superseded{0};
    // Candidates not decided on, by reason: before_training_cutoff,
    // not_after_watermark, decision_interval, inputs_unavailable:<reason>,
    // stale_signal.
    std::map<std::string, std::uint64_t> skipped;
    std::uint64_t predictions{0};
    std::uint64_t holds{0};
    std::uint64_t order_decisions{0};
    std::uint64_t orders_recorded{0};
    std::uint64_t abandoned{0};
    // Submission attempts that found no eligible quote yet, by reason.
    std::map<std::string, std::uint64_t> waits;
    std::int64_t last_inference_us{0};
    std::int64_t max_inference_us{0};
    // Wall time of each call to the trusted order path, including waiting
    // for the market-event gate, which a fast replay can hold for most of
    // its time.
    std::int64_t last_submission_us{0};
    std::int64_t max_submission_us{0};
    std::optional<std::string> last_error;
};

// The latest outcome of one of a runner's two activities, deciding on a
// candidate and checking or submitting its pending order, as the runner
// recorded it (ADR 0019). Kept in memory only.
struct DeploymentRuntimeOutcome {
    // A candidate outcome, such as before_training_cutoff, decision_interval,
    // inputs_unavailable:<reason>, decided_hold, or decided_order; or a
    // submission outcome, such as awaiting_latency, order_matching_busy,
    // recorded:<resolution>, abandoned:<reason>, or uncertain.
    std::string code;
    // The candidate's observation, for a candidate outcome.
    std::optional<std::uint64_t> ingest_sequence;
    std::optional<Timestamp> observation_receive;
    // The decision, for a decided candidate or a submission outcome.
    std::optional<std::int64_t> decision_sequence;
    // When it was recorded: the market clock, if it had started, and the
    // server's wall clock.
    std::optional<Timestamp> at_market;
    Timestamp at_wall;
};

// The order a runner is waiting to submit, as it holds it.
struct DeploymentPendingOrder {
    std::int64_t decision_sequence{0};
    Timestamp observation_receive;
    Timestamp not_before_receive;
    Timestamp expires_at_market;
};

struct DeploymentActivity {
    std::optional<DeploymentRuntimeOutcome> last_candidate;
    std::optional<DeploymentRuntimeOutcome> last_submission;
    std::optional<DeploymentPendingOrder> pending;
};

// The deployment instrument's execution quote, read when a status is
// rendered, in the market clock.
struct DeploymentQuoteCondition {
    bool available{false};
    // MarketState's reason when it is not available, such as
    // awaiting_fresh_quote.
    std::optional<std::string> unavailable_reason;
    std::optional<std::uint64_t> ingest_sequence;
    std::optional<Timestamp> received_at;
    // Market time minus the quote's receive time.
    std::optional<std::int64_t> age_ns;
    // The account's maximum quote age.
    std::int64_t maximum_age_ns{0};
    bool stale{false};
    bool suspect{false};
};

struct DeploymentMarketCondition {
    std::optional<Timestamp> market_time;
    // Absent when the server gave no source to read.
    std::optional<market_data::MarketDataSourceState> source_state;
    // The latest trading status of the instrument, or nullopt before any.
    std::optional<TradingStatus> trading_status;
    DeploymentQuoteCondition quote;
};

// What the deployment is doing now, as one explanation: the first of its
// lifecycle, the source, the quote, a pending order, and its latest
// candidate that applies (ADR 0019 gives the precedence).
struct DeploymentCondition {
    std::string code;
    // A more specific stable code: the lifecycle reason, the quote's
    // unavailability, a pending order's latest check, or a skip's detail.
    std::optional<std::string> reason;
    std::optional<Timestamp> observation_receive;
    std::optional<Timestamp> training_cutoff;
    std::optional<Timestamp> next_eligible_receive;
    std::optional<Timestamp> not_before_receive;
    std::optional<Timestamp> expires_at;
    std::optional<std::int64_t> decision_sequence;
    // The provenance of the fact behind it.
    std::optional<Timestamp> observed_at_market;
    std::optional<Timestamp> observed_at_wall;
};

[[nodiscard]] std::string_view source_state_code(market_data::MarketDataSourceState state) noexcept;

// The condition, from what is known. Pure, so every precedence is testable.
[[nodiscard]] DeploymentCondition deployment_condition(
    const ModelDeploymentRecord& deployment,
    bool active,
    const std::optional<DeploymentActivity>& activity,
    const std::optional<DeploymentMarketCondition>& market,
    Timestamp now_wall
);

struct DeploymentStatus {
    ModelDeploymentRecord deployment;
    // A runner is active for the deployment in this server process.
    bool active{false};
    // The current run's counters, while active or after it ended in this
    // process.
    std::optional<DeploymentRunCounters> run;
    // The runner's latest outcomes, while active or after it ended in this
    // process; null after a restart.
    std::optional<DeploymentActivity> activity{};
    // Read when the status was rendered; absent for another stream.
    std::optional<DeploymentMarketCondition> market{};
    DeploymentCondition condition{};
};

using DeploymentResult = std::variant<DeploymentStatus, DeploymentError>;

struct DeploymentCreated {
    DeploymentStatus status;
    bool created{false};
};

using DeploymentCreateResult = std::variant<DeploymentCreated, DeploymentError>;

struct DeploymentListPage {
    std::vector<DeploymentStatus> deployments;
    std::optional<std::int64_t> next_before_sequence;
};

using DeploymentListResult = std::variant<DeploymentListPage, DeploymentError>;
using DecisionListResult = std::variant<DecisionPage, DeploymentError>;

// Every deployment control in the server. Nothing is started on its own: at
// startup recover() stops whatever was running, and only start() runs a
// deployment.
//
// Without an inference provider, deployments are disabled: nothing can be
// created or started, while listing, Stop, and the kill switch still work,
// so a server started without the opt-in can still stop what another left.
class DeploymentManager final {
public:
    using Logger = std::function<void(std::string_view)>;

    DeploymentManager(
        PaperTradingService& service,
        DeploymentHub& hub,
        std::unique_ptr<InferenceProvider> provider,
        DeploymentRuntimeOptions options,
        Logger log = {}
    );
    ~DeploymentManager();

    DeploymentManager(const DeploymentManager&) = delete;
    DeploymentManager& operator=(const DeploymentManager&) = delete;

    // Before serving: every running or stopping deployment becomes stopped
    // (server_restarted) and every pending decision is abandoned.
    void recover();

    [[nodiscard]] DeploymentCreateResult create(const DeploymentCreateRequest& request);
    [[nodiscard]] DeploymentResult get(std::string_view deployment_id);
    [[nodiscard]] DeploymentListResult list(std::size_t limit, std::optional<std::int64_t> before_sequence);
    [[nodiscard]] DecisionListResult decisions(
        std::string_view deployment_id,
        std::size_t limit,
        std::optional<std::int64_t> before_sequence
    );
    // Idempotent: starting a running deployment returns it unchanged.
    [[nodiscard]] DeploymentResult start(std::string_view deployment_id);
    // Idempotent: stopping a stopped or faulted deployment returns it.
    // Positions are kept; nothing is liquidated.
    [[nodiscard]] DeploymentResult stop(std::string_view deployment_id, std::string_view reason = "stop_requested");
    // Stops every deployment with reason kill_switch. Idempotent.
    [[nodiscard]] std::variant<std::vector<DeploymentStatus>, DeploymentError> kill_switch();
    // Stops every runner, as at server shutdown.
    void shutdown();

    [[nodiscard]] bool enabled() const noexcept { return provider_ != nullptr; }

    // The canonical request whose SHA-256 identifies a create: version 1 for
    // an experiment's model, unchanged since ADR 0018, and version 2 for a
    // registered model version.
    [[nodiscard]] static std::string canonical_request(const DeploymentCreateRequest& request);

private:
    class Runner;

    [[nodiscard]] std::optional<DeploymentError> validate(const DeploymentCreateRequest& request) const;
    [[nodiscard]] DeploymentResult stop_locked(std::string_view deployment_id, std::string_view reason);
    [[nodiscard]] DeploymentStatus status_of(ModelDeploymentRecord record);
    [[nodiscard]] std::optional<DeploymentMarketCondition> market_condition_of(const ModelDeploymentConfig& config) const;
    void reap_finished_locked();

    PaperTradingService& service_;
    DeploymentHub& hub_;
    std::unique_ptr<InferenceProvider> provider_;
    DeploymentRuntimeOptions options_;
    Logger log_;
    // Serializes control requests. Never taken by a runner.
    std::mutex control_mutex_;
    // Guards runners_ for readers; a leaf.
    mutable std::mutex runners_mutex_;
    std::map<std::string, std::unique_ptr<Runner>> runners_;
    // The counters and latest outcomes of runs that ended in this process.
    std::map<std::string, DeploymentRunCounters> finished_runs_;
    std::map<std::string, DeploymentActivity> finished_activity_;
};

}  // namespace chronos::paper_trading
