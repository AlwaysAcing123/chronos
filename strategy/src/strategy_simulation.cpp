#include "chronos/strategy/strategy_simulation.hpp"

#include "chronos/strategy/long_flat_policy.hpp"

#include "chronos/canonical_json.hpp"
#include "chronos/market_quotes.hpp"
#include "chronos/paper_order_codes.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::strategy {
namespace {

namespace json = chronos::canonical_json;
using datasets::DatasetError;
using datasets::DatasetErrorCode;

json::Value decimal_text(std::int64_t value) { return json::Value(std::to_string(value)); }
json::Value count(std::uint64_t value) { return json::Value(std::to_string(value)); }
json::Value at_json(Timestamp at) { return decimal_text(at.nanoseconds()); }
json::Value optional_at(const std::optional<Timestamp>& at) {
    return at ? at_json(*at) : json::Value(nullptr);
}
template <typename Decimal>
json::Value fixed(Decimal value) {
    return json::Value(value.to_string());
}

json::Value record_name() {
    return json::Value(json::Object{{"name", kJournalRecordName}, {"version", kJournalRecordVersion}});
}

std::string_view valuation_error_name(PortfolioValuationErrorCode code) {
    switch (code) {
        case PortfolioValuationErrorCode::InvalidMarketState: return "invalid_market_state";
        case PortfolioValuationErrorCode::MarketInstrumentMismatch: return "market_instrument_mismatch";
        case PortfolioValuationErrorCode::SuspectMarketState: return "suspect_market_state";
        case PortfolioValuationErrorCode::MissingBestBid: return "missing_best_bid";
        case PortfolioValuationErrorCode::StaleMarketState: return "stale_market_state";
        case PortfolioValuationErrorCode::MarketTimelineMismatch: return "incompatible_market_timeline";
        case PortfolioValuationErrorCode::NonpositiveStartingCash: return "nonpositive_starting_cash";
        case PortfolioValuationErrorCode::ArithmeticOverflow: return "arithmetic_overflow";
        case PortfolioValuationErrorCode::AccountingIdentityFailed: return "accounting_identity_failed";
        case PortfolioValuationErrorCode::PositionMarkUnavailable: return "position_mark_unavailable";
    }
    throw std::logic_error("unknown portfolio valuation error");
}

std::string_view market_state_name(MarketStateUnavailableCode code) {
    switch (code) {
        case MarketStateUnavailableCode::StreamInterrupted: return "stream_interrupted";
        case MarketStateUnavailableCode::NoTopOfBook: return "no_top_of_book";
        case MarketStateUnavailableCode::SourceUnavailable: return "source_unavailable";
        case MarketStateUnavailableCode::AwaitingFreshQuote: return "awaiting_fresh_quote";
    }
    throw std::logic_error("unknown market-state unavailability");
}

json::Value level_json(const std::optional<BookLevel>& level) {
    if (!level) {
        return json::Value(nullptr);
    }
    return json::Value(json::Object{{"price", fixed(level->price)}, {"quantity", fixed(level->quantity)}});
}

json::Value observation_json(const TopOfBookEvent& quote) {
    return json::Value(json::Object{
        {"ask", level_json(quote.ask)},
        {"bid", level_json(quote.bid)},
        {"ingest_sequence", count(quote.header.ingest_sequence)},
        {"receive_timestamp_ns", at_json(quote.header.receive_timestamp)},
        {"suspect", quote.header.quality.is_suspect()},
    });
}

json::Value valuation_json(const PortfolioValuationResult& result, const InstrumentId& instrument) {
    if (const auto* error = std::get_if<PortfolioValuationError>(&result)) {
        json::Array instruments;
        for (const auto& failure : error->instruments) {
            const std::string_view reason = std::visit(
                [](auto code) -> std::string_view {
                    if constexpr (std::is_same_v<decltype(code), MarketStateUnavailableCode>) {
                        return market_state_name(code);
                    } else {
                        return valuation_error_name(code);
                    }
                },
                failure.reason
            );
            instruments.push_back(json::Value(json::Object{
                {"instrument", failure.instrument.value()},
                {"message", failure.message},
                {"reason", reason},
            }));
        }
        return json::Value(json::Object{{"unavailable", json::Object{
            {"code", valuation_error_name(error->code)},
            {"instruments", std::move(instruments)},
            {"message", error->message},
        }}});
    }
    const auto& valuation = std::get<PortfolioValuation>(result);
    const AccountSnapshot& snapshot = valuation.snapshot;
    json::Value mark(nullptr);
    if (const auto found = valuation.marks.find(instrument); found != valuation.marks.end() && found->second.bid) {
        mark = json::Value(json::Object{
            {"bid", fixed(found->second.bid->price)},
            {"ingest_sequence", count(found->second.header.ingest_sequence)},
            {"receive_timestamp_ns", at_json(found->second.header.receive_timestamp)},
        });
    }
    return json::Value(json::Object{
        {"cash", fixed(snapshot.state.cash)},
        {"equity", fixed(snapshot.equity)},
        {"fees_paid", fixed(snapshot.fees_paid)},
        {"gross_realized_pnl", fixed(snapshot.gross_realized_pnl)},
        {"gross_unrealized_pnl", fixed(snapshot.gross_unrealized_pnl)},
        {"mark", std::move(mark)},
        {"market_value", fixed(snapshot.market_value)},
        {"net_total_pnl", fixed(snapshot.net_total_pnl)},
        {"position", fixed(snapshot.state.quantity(instrument))},
        {"return_percent", fixed(valuation.return_percent)},
    });
}

std::string order_id(AccountRole role, std::uint64_t number) {
    std::string digits = std::to_string(number);
    if (digits.size() < 6) {
        digits.insert(0, 6 - digits.size(), '0');
    }
    return std::string(account_name(role)) + "-" + digits;
}

struct PendingOrder {
    std::string client_order_id;
    Side side{Side::Buy};
    Quantity quantity;
    Timestamp signal_at;
    Timestamp arrival_at;
};

struct Account {
    AccountRole role;
    PaperOrderProcessor processor;
    std::optional<PendingOrder> pending;
    std::uint64_t orders_created{0};
    OrderCounts orders;
    std::uint64_t fills{0};
    Quantity traded_quantity;
    Money turnover;
    std::optional<Money> peak;
    std::optional<Timestamp> peak_at;
    Drawdown drawdown;
    PortfolioValuationResult last_valuation{PortfolioValuationError{
        .code = PortfolioValuationErrorCode::InvalidMarketState,
        .message = "not yet valued",
        .instruments = {},
    }};
};

DatasetError run_error(DatasetErrorCode code, std::string field, std::string message) {
    return datasets::dataset_error(code, std::move(field), std::move(message));
}

}  // namespace

std::optional<std::string> configuration_error(const StrategyConfiguration& configuration) {
    if (configuration.target_quantity.raw() <= 0) {
        return "the target quantity must be positive";
    }
    if (configuration.threshold_ppb > kConfidenceScale) {
        return "the threshold must be at most 1000000000 parts per billion";
    }
    if (configuration.latency_ns <= 0) {
        return "the latency must be positive, so a signal can never fill on its own quote";
    }
    if (configuration.decision_interval_ns <= 0) {
        return "the decision interval must be positive";
    }
    if (configuration.starting_cash.raw() <= 0) {
        return "the starting cash must be positive";
    }
    if (configuration.fee_per_fill.raw() < 0) {
        return "the fee per fill must not be negative";
    }
    if (configuration.maximum_quote_age_ns <= 0) {
        return "the maximum quote age must be positive";
    }
    if (configuration.maximum_order_notional.raw() <= 0) {
        return "the maximum order notional must be positive";
    }
    return std::nullopt;
}

json::Value configuration_json(const StrategyConfiguration& configuration) {
    return json::Value(json::Object{
        {"execution", json::Object{
            {"fee_per_fill", fixed(configuration.fee_per_fill)},
            {"latency_ns", decimal_text(configuration.latency_ns)},
            {"maximum_order_notional", fixed(configuration.maximum_order_notional)},
            {"maximum_quote_age_ns", decimal_text(configuration.maximum_quote_age_ns)},
            {"order_type", "market"},
            {"time_in_force", "immediate_or_cancel"},
        }},
        {"policy", json::Object{
            {"decision_interval_ns", decimal_text(configuration.decision_interval_ns)},
            {"name", kPolicyName},
            {"target_quantity", fixed(configuration.target_quantity)},
            {"threshold_ppb", count(configuration.threshold_ppb)},
            {"version", kPolicyVersion},
        }},
        {"starting_cash", fixed(configuration.starting_cash)},
    });
}

std::string_view account_name(AccountRole role) noexcept {
    switch (role) {
        case AccountRole::Strategy: return "strategy";
        case AccountRole::BuyAndHold: return "buy_and_hold";
        case AccountRole::Cash: return "cash";
    }
    return "strategy";
}

json::Value results_json(const StrategyResults& results, const StrategyConfiguration& configuration) {
    const InstrumentId instrument(results.instrument);
    json::Object accounts;
    for (const AccountResult& account : results.accounts) {
        const OrderCounts& orders = account.orders;
        const Drawdown& drawdown = account.drawdown;
        json::Object ledger{
            {"cash", fixed(account.final_state.cash)},
            {"fees_paid", fixed(account.final_state.fees_paid())},
            {"gross_realized_pnl", fixed(account.final_state.gross_realized_pnl())},
            {"open_cost_basis", fixed(account.final_state.open_cost_basis())},
            {"position", fixed(account.final_state.quantity(instrument))},
        };
        json::Object rejections;
        for (const auto& [code, n] : orders.rejections) {
            rejections.emplace_back(code, count(n));
        }
        accounts.emplace_back(std::string(account_name(account.role)), json::Object{
            {"drawdown", json::Object{
                {"maximum", fixed(drawdown.maximum)},
                {"maximum_percent_of_peak",
                 drawdown.maximum_percent_of_peak ? fixed(*drawdown.maximum_percent_of_peak) : json::Value(nullptr)},
                {"peak_at_ns", optional_at(drawdown.peak_at)},
                {"points_unvalued", count(drawdown.points_unvalued)},
                {"points_valued", count(drawdown.points_valued)},
                {"trough_at_ns", optional_at(drawdown.trough_at)},
            }},
            {"fills", count(account.fills)},
            {"final", json::Object{
                {"ledger", std::move(ledger)},
                {"valuation", valuation_json(account.final_valuation, instrument)},
            }},
            {"orders", json::Object{
                {"cancelled", count(orders.cancelled)},
                {"expired", count(orders.expired)},
                {"filled", count(orders.filled)},
                {"partially_filled_and_cancelled", count(orders.partially_filled_and_cancelled)},
                {"rejected", count(orders.rejected)},
                {"rejections", std::move(rejections)},
                {"scheduled", count(orders.scheduled)},
            }},
            {"traded_quantity", fixed(account.traded_quantity)},
            {"turnover", json::Object{
                {"notional", fixed(account.turnover)},
                {"percent_of_starting_cash", fixed(percentage_of(account.turnover, configuration.starting_cash))},
            }},
        });
    }
    const DecisionCounts& decisions = results.decisions;
    return json::Value(json::Object{
        {"accounts", std::move(accounts)},
        {"decisions", json::Object{
            {"already_at_target", count(decisions.already_at_target)},
            {"evaluated", count(decisions.evaluated)},
            {"hold_below_threshold", count(decisions.hold_below_threshold)},
            {"hold_flat_prediction", count(decisions.hold_flat_prediction)},
            {"order_pending", count(decisions.order_pending)},
            {"orders", count(decisions.orders)},
            {"skipped_decision_interval", count(decisions.skipped_decision_interval)},
            {"skipped_prediction_unavailable", count(decisions.skipped_prediction_unavailable)},
        }},
        {"events", json::Object{
            {"schedule_rows", count(results.schedule_rows)},
            {"warmup", count(results.warmup_events)},
            {"window", count(results.window_events)},
        }},
        {"first_window_event_at_ns", optional_at(results.first_window_event_at)},
        {"valued_at_ns", optional_at(results.valued_at)},
    });
}

class StrategySimulation::State final {
public:
    State(
        StrategyConfiguration configuration_,
        InstrumentId instrument_,
        MarketDataStreamId stream_,
        ScheduleWindow window_,
        std::string source_id_,
        JournalSink journal_
    )
        : configuration(configuration_),
          instrument(std::move(instrument_)),
          stream(std::move(stream_)),
          window(std::move(window_)),
          source_id(std::move(source_id_)),
          journal(std::move(journal_)) {
        const PreTradeRiskLimits limits{
            .maximum_order_quantity = configuration.target_quantity,
            .maximum_order_notional = configuration.maximum_order_notional,
            .maximum_position_quantity = configuration.target_quantity,
            .maximum_quote_age_nanoseconds = configuration.maximum_quote_age_ns,
            .fee_per_fill = configuration.fee_per_fill,
        };
        for (AccountRole role : kAccountRoles) {
            accounts.push_back(Account{
                .role = role,
                .processor = PaperOrderProcessor(std::string(account_name(role)), configuration.starting_cash, limits),
                .pending = std::nullopt,
                .orders_created = 0,
                .orders = {},
                .fills = 0,
                .traded_quantity = {},
                .turnover = {},
                .peak = std::nullopt,
                .peak_at = std::nullopt,
                .drawdown = {},
            });
        }
    }

    Account& account(AccountRole role) { return accounts[static_cast<std::size_t>(role)]; }

    std::optional<DatasetError> write(json::Object entry) {
        entry.emplace_back("record", record_name());
        return journal(json::Value(std::move(entry)));
    }

    std::optional<DatasetError> schedule_order(
        Account& owner,
        Side side,
        Quantity quantity,
        Timestamp signal_at,
        std::uint64_t decision
    ) {
        std::int64_t arrival = 0;
        if (__builtin_add_overflow(signal_at.nanoseconds(), configuration.latency_ns, &arrival)) {
            return run_error(DatasetErrorCode::CounterOverflow, "latency_ns", "the order's arrival time overflows");
        }
        owner.pending = PendingOrder{
            .client_order_id = order_id(owner.role, ++owner.orders_created),
            .side = side,
            .quantity = quantity,
            .signal_at = signal_at,
            .arrival_at = Timestamp::from_nanoseconds(arrival),
        };
        ++owner.orders.scheduled;
        return write(json::Object{
            {"account", account_name(owner.role)},
            {"arrival_at_ns", at_json(owner.pending->arrival_at)},
            {"client_order_id", owner.pending->client_order_id},
            {"decision", count(decision)},
            {"quantity", fixed(quantity)},
            {"side", *canonical_name(side)},
            {"signal_at_ns", at_json(signal_at)},
            {"type", "order"},
        });
    }

    std::optional<DatasetError> execute(Account& owner, const TopOfBookEvent& quote) {
        const PendingOrder order = *owner.pending;
        owner.pending.reset();
        json::Value fill(nullptr);
        json::Value rejection(nullptr);
        json::Value cancellation(nullptr);
        std::string_view outcome = "rejected";
        if (trading_status == TradingStatus::Halted || trading_status == TradingStatus::Closed) {
            const std::string_view code = trading_status == TradingStatus::Halted ? "trading_halted" : "market_closed";
            ++owner.orders.rejected;
            ++owner.orders.rejections[std::string(code)];
            rejection = json::Value(json::Object{
                {"by", "runner"},
                {"code", code},
                {"message", "the instrument's latest trading status at the execution observation"},
            });
        } else {
            const OrderIntent intent{
                .client_order_id = order.client_order_id,
                .account_id = std::string(account_name(owner.role)),
                .source = OrderSource{.kind = OrderSourceKind::Model, .source_id = source_id},
                .instrument = instrument,
                .side = order.side,
                .type = OrderType::Market,
                .quantity = order.quantity,
                .limit_price = std::nullopt,
                .time_in_force = TimeInForce::ImmediateOrCancel,
                .submitted_at = quote.header.receive_timestamp,
            };
            PaperOrderResult result = owner.processor.submit(intent, quote);
            if (const auto* receipt = std::get_if<PaperOrderReceipt>(&result)) {
                const SimulatedExecutionReport& report = receipt->execution;
                switch (report.status) {
                    case SimulatedExecutionStatus::Filled:
                        ++owner.orders.filled;
                        outcome = "filled";
                        break;
                    case SimulatedExecutionStatus::PartiallyFilledAndCancelled:
                        ++owner.orders.partially_filled_and_cancelled;
                        outcome = "partially_filled_and_cancelled";
                        break;
                    case SimulatedExecutionStatus::Cancelled:
                        ++owner.orders.cancelled;
                        outcome = "cancelled";
                        break;
                    case SimulatedExecutionStatus::Working:
                    case SimulatedExecutionStatus::PartiallyFilled:
                        return run_error(DatasetErrorCode::InvalidValue, "execution", "an immediate-or-cancel order rested");
                }
                if (report.cancellation_reason) {
                    cancellation = json::Value(std::string(*canonical_name(*report.cancellation_reason)));
                }
                if (receipt->accounting) {
                    const ApplyFillReceipt& booked = *receipt->accounting;
                    ++owner.fills;
                    owner.traded_quantity = owner.traded_quantity + booked.fill.quantity;
                    owner.turnover = owner.turnover + booked.notional;
                    fill = json::Value(json::Object{
                        {"fee", fixed(booked.fill.fee)},
                        {"fill_id", booked.fill.fill_id},
                        {"notional", fixed(booked.notional)},
                        {"price", fixed(booked.fill.price)},
                        {"quantity", fixed(booked.fill.quantity)},
                    });
                }
            } else if (const auto* rejected = std::get_if<PreTradeRiskRejection>(&result)) {
                const std::string code(*canonical_name(rejected->code));
                ++owner.orders.rejected;
                ++owner.orders.rejections[code];
                rejection = json::Value(json::Object{
                    {"by", "pre_trade_risk"},
                    {"code", code},
                    {"message", rejected->message},
                });
            } else if (const auto* failed = std::get_if<SimulatedExecutionError>(&result)) {
                return run_error(DatasetErrorCode::InvalidValue, "execution", "the simulator refused the order: " + failed->message);
            } else {
                return run_error(
                    DatasetErrorCode::InvalidValue,
                    "execution",
                    "the paper order processor faulted: " + std::get<PaperOrderProcessingError>(result).message
                );
            }
        }
        const AccountState& after = owner.processor.account().state();
        return write(json::Object{
            {"account", account_name(owner.role)},
            {"arrival_at_ns", at_json(order.arrival_at)},
            {"cancellation_reason", std::move(cancellation)},
            {"cash_after", fixed(after.cash)},
            {"client_order_id", order.client_order_id},
            {"executed_at_ns", at_json(quote.header.receive_timestamp)},
            {"fill", std::move(fill)},
            {"observation", observation_json(quote)},
            {"outcome", outcome},
            {"position_after", fixed(after.quantity(instrument))},
            {"rejection", std::move(rejection)},
            {"signal_at_ns", at_json(order.signal_at)},
            {"type", "execution"},
        });
    }

    std::optional<DatasetError> decide(const ScheduleRow& row, Timestamp at, bool& decided) {
        if (!row.prediction) {
            ++results.decisions.skipped_prediction_unavailable;
            return std::nullopt;
        }
        if (last_decision_at && at.nanoseconds() - last_decision_at->nanoseconds() < configuration.decision_interval_ns) {
            ++results.decisions.skipped_decision_interval;
            return std::nullopt;
        }
        decided = true;
        last_decision_at = at;
        const std::uint64_t decision = ++results.decisions.evaluated;
        Account& strategy = account(AccountRole::Strategy);
        const Quantity position = strategy.processor.account().state().quantity(instrument);
        const SchedulePrediction& prediction = *row.prediction;
        const PolicyDecision policy = decide_long_flat(
            prediction, configuration.threshold_ppb, configuration.target_quantity, position, strategy.pending.has_value()
        );
        const std::optional<Quantity>& target = policy.target;
        const std::string_view reason = policy_reason_name(policy.reason);
        switch (policy.reason) {
            case PolicyReason::HoldFlatPrediction: ++results.decisions.hold_flat_prediction; break;
            case PolicyReason::HoldBelowThreshold: ++results.decisions.hold_below_threshold; break;
            case PolicyReason::OrderPending: ++results.decisions.order_pending; break;
            case PolicyReason::AlreadyAtTarget: ++results.decisions.already_at_target; break;
            case PolicyReason::OrderScheduled: ++results.decisions.orders; break;
        }
        const json::Value signal(json::Object{
            {"available_at_ns", at_json(row.available_at)},
            {"class", predicted_class_name(prediction.predicted_class)},
            {"confidence_ppb", count(prediction.confidence_ppb)},
            {"ingest_sequence", count(row.ingest_sequence)},
        });
        if (auto error = write(json::Object{
                {"account", account_name(AccountRole::Strategy)},
                {"decision", count(decision)},
                {"position_before", fixed(position)},
                {"reason", reason},
                {"signal", signal},
                {"target", target ? fixed(*target) : json::Value(nullptr)},
                {"type", "decision"},
            })) {
            return error;
        }
        if (policy.order) {
            if (auto error = schedule_order(strategy, policy.order->side, policy.order->quantity, at, decision)) {
                return error;
            }
        }
        if (!benchmark_entered) {
            // Buy and hold enters once, at the strategy's first decision, under
            // the same latency, execution, fee, and valuation rules.
            benchmark_entered = true;
            Account& benchmark = account(AccountRole::BuyAndHold);
            if (auto error = write(json::Object{
                    {"account", account_name(AccountRole::BuyAndHold)},
                    {"decision", count(decision)},
                    {"position_before", fixed(Quantity{})},
                    {"reason", "benchmark_entry"},
                    {"signal", signal},
                    {"target", fixed(configuration.target_quantity)},
                    {"type", "decision"},
                })) {
                return error;
            }
            if (auto error = schedule_order(benchmark, Side::Buy, configuration.target_quantity, at, decision)) {
                return error;
            }
        }
        return std::nullopt;
    }

    std::optional<DatasetError> value(Timestamp at, std::string_view trigger) {
        MarketQuotes quotes{.stream = stream, .market_time = at, .quotes = {}};
        if (latest_quote) {
            quotes.quotes.emplace(instrument, InstrumentQuote(*latest_quote));
        } else {
            quotes.quotes.emplace(instrument, InstrumentQuote(MarketStateUnavailable{
                .code = MarketStateUnavailableCode::NoTopOfBook,
                .message = "the instrument has no quote yet",
            }));
        }
        json::Object valued;
        for (Account& owner : accounts) {
            owner.last_valuation = value_portfolio(
                owner.processor.account(), owner.processor.executions(), quotes, configuration.maximum_quote_age_ns
            );
            Drawdown& drawdown = owner.drawdown;
            if (const auto* valuation = std::get_if<PortfolioValuation>(&owner.last_valuation)) {
                ++drawdown.points_valued;
                const Money equity = valuation->snapshot.equity;
                if (!owner.peak || equity > *owner.peak) {
                    owner.peak = equity;
                    owner.peak_at = at;
                }
                const Money fall = *owner.peak - equity;
                if (fall > drawdown.maximum) {
                    drawdown.maximum = fall;
                    drawdown.maximum_percent_of_peak = owner.peak->raw() > 0
                        ? std::optional(percentage_of(fall, *owner.peak))
                        : std::nullopt;
                    drawdown.peak_at = owner.peak_at;
                    drawdown.trough_at = at;
                }
            } else {
                ++drawdown.points_unvalued;
            }
            valued.emplace_back(std::string(account_name(owner.role)), valuation_json(owner.last_valuation, instrument));
        }
        return write(json::Object{
            {"accounts", std::move(valued)},
            {"at_ns", at_json(at)},
            {"trigger", trigger},
            {"type", "valuation"},
        });
    }

    std::optional<DatasetError> apply(const ValidatedMarketEvent& event, const ScheduleRowSource& rows) {
        if (finished) {
            return run_error(DatasetErrorCode::InvalidArgument, "event", "the simulation has finished");
        }
        const MarketEventHeader& header = market_event_header(event.value());
        if (header.stream != stream) {
            return run_error(DatasetErrorCode::StreamMismatch, "stream", "the event is not on the schedule's stream");
        }
        if ((last_sequence && header.ingest_sequence <= *last_sequence) ||
            (last_event_at && header.receive_timestamp < *last_event_at)) {
            return run_error(DatasetErrorCode::IngestSequenceMovedBackward, "ingest_sequence", "events must follow ingest order");
        }
        if (window.ended_by(header.receive_timestamp)) {
            return run_error(DatasetErrorCode::InvalidArgument, "receive_timestamp_ns", "events at or after the window's end are never applied");
        }
        last_sequence = header.ingest_sequence;
        last_event_at = header.receive_timestamp;
        const bool in_window = window.contains(header.receive_timestamp);
        if (in_window) {
            ++results.window_events;
            if (!results.first_window_event_at) {
                results.first_window_event_at = header.receive_timestamp;
            }
        } else {
            ++results.warmup_events;
        }

        const std::optional<features::TopOfBookFeatureRow> feature = engine.apply(event);
        if (const auto* status_event = std::get_if<MarketStatusEvent>(&event.value());
            status_event != nullptr && status_event->header.instrument == instrument) {
            trading_status = status_event->status;
        }
        bool executed = false;
        if (const auto* quote = std::get_if<TopOfBookEvent>(&event.value());
            quote != nullptr && quote->header.instrument == instrument) {
            latest_quote = *quote;
            for (AccountRole role : {AccountRole::Strategy, AccountRole::BuyAndHold}) {
                Account& owner = account(role);
                if (owner.pending && header.receive_timestamp >= owner.pending->arrival_at) {
                    executed = true;
                    if (auto error = execute(owner, *quote)) {
                        return error;
                    }
                }
            }
        }
        bool decided = false;
        if (feature && feature->instrument == instrument && in_window) {
            auto next = rows();
            if (auto* error = std::get_if<DatasetError>(&next)) {
                return std::move(*error);
            }
            if (std::holds_alternative<datasets::EndOfRows>(next)) {
                return run_error(
                    DatasetErrorCode::CountMismatch,
                    "schedule.ndjson",
                    "the schedule ends before the window's top-of-book events of the instrument do"
                );
            }
            const ScheduleRow& row = std::get<ScheduleRow>(next);
            ++results.schedule_rows;
            if (row.ingest_sequence != feature->ingest_sequence || row.available_at != feature->available_at ||
                row.inputs != model_inputs(*feature)) {
                return datasets::dataset_error(
                    DatasetErrorCode::IdentityMismatch,
                    "schedule.ndjson",
                    "a row differs from the features recomputed from the dataset at ingest sequence " +
                        std::to_string(feature->ingest_sequence),
                    results.schedule_rows
                );
            }
            if (auto error = decide(row, header.receive_timestamp, decided)) {
                return error;
            }
        }
        if (decided || executed) {
            return value(header.receive_timestamp, decided ? "decision" : "execution");
        }
        return std::nullopt;
    }

    std::variant<StrategyResults, DatasetError> finish() {
        if (finished) {
            return run_error(DatasetErrorCode::InvalidArgument, "event", "the simulation has finished");
        }
        finished = true;
        if (!results.first_window_event_at || !last_event_at) {
            return run_error(DatasetErrorCode::EmptyDataset, "window", "no event of the dataset falls inside the window");
        }
        for (Account& owner : accounts) {
            if (owner.pending) {
                ++owner.orders.expired;
                const PendingOrder order = *owner.pending;
                owner.pending.reset();
                if (auto error = write(json::Object{
                        {"account", account_name(owner.role)},
                        {"arrival_at_ns", at_json(order.arrival_at)},
                        {"client_order_id", order.client_order_id},
                        {"reason", "window_ended_before_an_eligible_observation"},
                        {"type", "order_expired"},
                    })) {
                    return std::move(*error);
                }
            }
        }
        if (auto error = value(*last_event_at, "window_end")) {
            return std::move(*error);
        }
        results.valued_at = last_event_at;
        results.instrument = instrument.value();
        for (std::size_t index = 0; index < accounts.size(); ++index) {
            Account& owner = accounts[index];
            results.accounts[index] = AccountResult{
                .role = owner.role,
                .final_state = owner.processor.account().state(),
                .final_valuation = owner.last_valuation,
                .orders = owner.orders,
                .fills = owner.fills,
                .traded_quantity = owner.traded_quantity,
                .turnover = owner.turnover,
                .drawdown = owner.drawdown,
            };
        }
        return results;
    }

    StrategyConfiguration configuration;
    InstrumentId instrument;
    MarketDataStreamId stream;
    ScheduleWindow window;
    std::string source_id;
    JournalSink journal;
    features::TopOfBookFeatureEngine engine;
    std::vector<Account> accounts;
    std::optional<TopOfBookEvent> latest_quote;
    std::optional<TradingStatus> trading_status;
    std::optional<Timestamp> last_decision_at;
    std::optional<Timestamp> last_event_at;
    std::optional<std::uint64_t> last_sequence;
    bool benchmark_entered{false};
    bool finished{false};
    StrategyResults results;
};

StrategySimulation::StrategySimulation(
    StrategyConfiguration configuration,
    InstrumentId instrument,
    MarketDataStreamId stream,
    ScheduleWindow window,
    std::string source_id,
    JournalSink journal
) {
    if (auto error = configuration_error(configuration)) {
        throw std::invalid_argument(*error);
    }
    state_ = std::make_unique<State>(
        configuration, std::move(instrument), std::move(stream), std::move(window), std::move(source_id),
        std::move(journal)
    );
}

StrategySimulation::~StrategySimulation() = default;

bool StrategySimulation::accepts(const ValidatedMarketEvent& event) const noexcept {
    return !state_->window.ended_by(market_event_header(event.value()).receive_timestamp);
}

std::optional<DatasetError> StrategySimulation::apply(const ValidatedMarketEvent& event, const ScheduleRowSource& rows) {
    try {
        return state_->apply(event, rows);
    } catch (const std::overflow_error& error) {
        return run_error(DatasetErrorCode::CounterOverflow, "accounting", error.what());
    }
}

std::variant<StrategyResults, DatasetError> StrategySimulation::finish() {
    try {
        return state_->finish();
    } catch (const std::overflow_error& error) {
        return run_error(DatasetErrorCode::CounterOverflow, "accounting", error.what());
    }
}

}  // namespace chronos::strategy
