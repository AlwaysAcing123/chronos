#pragma once

#include "chronos/market_data_types.hpp"
#include "chronos/pre_trade_risk.hpp"
#include "chronos/trading_types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The durable records of model deployments (migrations 005 and 008, ADRs
// 0018 and 0021). The repository that stores them is
// PostgresPaperOrderRepository: a deployment's account is a paper account,
// and its orders commit in the account's own transaction.
namespace chronos {

inline constexpr std::string_view kDeploymentModelFeatureSet = "top_of_book_baseline/1";
inline constexpr std::string_view kDeploymentPolicy = "long_flat_threshold/1";
inline constexpr std::size_t kModelInputCount = 5;

enum class DeploymentState {
    Stopped,
    Running,
    Stopping,
    Faulted,
};

[[nodiscard]] std::string_view deployment_state_name(DeploymentState state) noexcept;
[[nodiscard]] std::optional<DeploymentState> deployment_state_from_name(std::string_view name) noexcept;

// An experiment's fitted model (ADR 0018).
struct ExperimentModel {
    std::string experiment_id;
    // "logistic" or "majority".
    std::string name;

    friend bool operator==(const ExperimentModel&, const ExperimentModel&) = default;
};

// A registered model version (migration 008, ADR 0021), by its immutable ID;
// never by a name, which could be bound to another version later.
struct RegisteredModel {
    std::string model_version_id;

    friend bool operator==(const RegisteredModel&, const RegisteredModel&) = default;
};

// Which model a deployment serves: exactly one of the two.
using ModelChoice = std::variant<ExperimentModel, RegisteredModel>;

// Everything a deployment is created with. None of it changes afterwards.
struct ModelDeploymentConfig {
    std::string deployment_id;
    std::string account_id;
    Money starting_cash;
    // The model, as the inference worker verified and identified it: its
    // choice, its family (logistic, majority, or a registered version's
    // own, such as onnx), and the checksum of the parameters or payload.
    ModelChoice model;
    std::string family;
    std::string parameters_sha256;
    InstrumentId instrument{"unset"};
    // No decision on an observation received before this.
    Timestamp training_cutoff;
    // verified, or declared by an import's uploader and not verified.
    std::string training_cutoff_basis{"verified"};
    // The policy, long_flat_threshold version 1.
    std::uint64_t threshold_ppb{0};
    Quantity target_quantity;
    // Receive-time cadence, the minimum signal-to-order latency, and how
    // long a signal stays actionable, all in market time.
    std::int64_t decision_interval_ns{0};
    std::int64_t latency_ns{0};
    std::int64_t max_signal_age_ns{0};
    // Safety limits; reaching one faults the deployment.
    std::int32_t max_orders_per_run{0};
    std::int32_t max_consecutive_rejections{0};
    // The one stream it may trade on.
    MarketDataStreamId stream{"unset"};
    // The new account's risk limits.
    PreTradeRiskLimits risk_limits;
    // The SHA-256 of the canonical creation request.
    std::string request_sha256;
};

struct ModelDeploymentRecord {
    ModelDeploymentConfig config;
    std::int64_t creation_sequence{0};
    DeploymentState state{DeploymentState::Stopped};
    std::string state_reason;
    // RFC 3339, as PostgreSQL renders it in UTC.
    std::string state_changed_at;
    std::string created_at;
    std::int64_t run_generation{0};
    std::int64_t next_decision_sequence{1};
    std::optional<Timestamp> watermark_receive;
    std::optional<std::uint64_t> watermark_ingest_sequence;
    std::int32_t orders_this_run{0};
    std::int32_t consecutive_rejections{0};
    std::int64_t version{0};
    // The decision still awaiting resolution, if any.
    std::optional<std::int64_t> pending_decision;
};

enum class DecisionState {
    // No order was intended.
    Held,
    // An order is intended and nothing is recorded yet.
    Pending,
    // Its order is in the journal.
    Recorded,
    // No order was, or ever will be, recorded for it.
    Abandoned,
};

[[nodiscard]] std::string_view decision_state_name(DecisionState state) noexcept;

// What the deployment knew and decided, before anything acted on it.
struct NewDeploymentDecision {
    std::string deployment_id;
    std::int64_t run_generation{0};
    MarketDataStreamId stream{"unset"};
    InstrumentId instrument{"unset"};
    std::uint64_t observation_ingest_sequence{0};
    Timestamp observation_event;
    Timestamp observation_receive;
    Timestamp decided_at_market;
    std::array<std::int64_t, kModelInputCount> inputs{};
    std::string predicted_class;
    std::array<std::string, 3> probabilities;
    std::uint64_t confidence_ppb{0};
    Quantity position_before;
    std::optional<Quantity> target;
    std::string policy_reason;
    // An order decision's intended order; absent for a hold.
    std::optional<Side> side;
    std::optional<Quantity> quantity;
};

// The booked fills of a decision's one order, summed exactly. Read only on
// decision pages; a decision whose order booked nothing has none.
struct DecisionFill {
    std::int64_t count{0};
    Quantity quantity;
    Money notional;
    Money fees;
    // Market time, as the fills record it.
    Timestamp first_filled_at;
    Timestamp last_filled_at;
    // Exactly when there was one fill; no average is computed.
    std::optional<Price> price;
};

struct DeploymentDecisionRecord {
    NewDeploymentDecision decision;
    std::int64_t decision_sequence{0};
    DecisionState state{DecisionState::Held};
    // `<deployment>-<12-digit sequence>`; present for an order decision.
    std::optional<std::string> client_order_id;
    std::optional<Timestamp> not_before_receive;
    std::optional<Timestamp> expires_at_market;
    // Recorded: the order's outcome. Abandoned: why.
    std::optional<std::string> resolution;
    std::optional<std::uint64_t> execution_ingest_sequence;
    std::optional<Timestamp> execution_receive;
    std::optional<Timestamp> resolved_at_market;
    std::string created_at;
    // On decision pages only; absent elsewhere and when nothing was booked.
    std::optional<DecisionFill> fill{};
};

// How a deployment order ended, recorded with it in one transaction.
struct DeploymentOrderResolution {
    std::string outcome;
    bool rejected{false};
    std::uint64_t execution_ingest_sequence{0};
    Timestamp execution_receive;
    Timestamp resolved_at_market;
};

// A deployment and one of its decisions, locked under an account lock.
struct LockedDeploymentDecision {
    ModelDeploymentRecord deployment;
    DeploymentDecisionRecord decision;
};

struct DeploymentPage {
    std::vector<ModelDeploymentRecord> deployments;
    // Present when older deployments exist: the last one's sequence.
    std::optional<std::int64_t> next_before_sequence;
};

struct DecisionPage {
    std::vector<DeploymentDecisionRecord> decisions;
    std::optional<std::int64_t> next_before_sequence;
};

enum class DeploymentConflictCode {
    // The deployment ID exists with a different request.
    DeploymentExists,
    // The account ID is taken.
    AccountExists,
    DeploymentNotFound,
    // The current state does not allow the transition.
    InvalidTransition,
    // A decision cannot be recorded: the deployment is not running in this
    // generation, a decision is unresolved, or the observation is not after
    // the watermark.
    DecisionRefused,
};

struct DeploymentConflict {
    DeploymentConflictCode code;
    std::string message;
    std::optional<ModelDeploymentRecord> deployment;
};

}  // namespace chronos
