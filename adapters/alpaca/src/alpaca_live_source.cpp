#include "chronos/alpaca/alpaca_live_source.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::alpaca_adapter {
namespace {

using market_data::MarketDataSourceState;
using Clock = market_data::MonotonicClock;

std::string seconds_text(Clock::duration duration) {
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    std::string fraction = std::to_string(milliseconds % 1000);
    fraction.insert(0, 3 - fraction.size(), '0');
    return std::to_string(milliseconds / 1000) + "." + fraction + " s";
}

std::string alpaca_error_text(const AlpacaErrorMessage& error) {
    return "Alpaca error " + std::to_string(error.code) + " (" +
        printable_excerpt(error.message) + ")";
}

std::string_view control_kind(const AlpacaMessage& message) {
    if (std::holds_alternative<AlpacaSuccess>(message)) {
        return "success";
    }
    if (std::holds_alternative<AlpacaErrorMessage>(message)) {
        return "error";
    }
    return "subscription";
}

std::string validation_details(const AlpacaNormalizationError& error) {
    std::string details = error.message;
    for (const MarketEventValidationError& violation : error.validation_errors) {
        details += "; " + violation.field + ": " + violation.message;
    }
    return details;
}

std::string sequence_diagnostic(const market_data::IngestSequencesUnavailable& unavailable) {
    return "ingest sequences unavailable: " + unavailable.diagnostic;
}

}  // namespace

// One connection's handshake and live stream, on the source's thread.
class AlpacaLiveSource::Session final {
public:
    Session(AlpacaLiveSource& source, AlpacaConnection& connection)
        : source_(source),
          connection_(connection) {}

    SessionEnd run() {
        auto greeting = await_control(
            source_.options_.greeting_timeout,
            "Alpaca's connection greeting"
        );
        if (auto* end = std::get_if<SessionEnd>(&greeting)) {
            return std::move(*end);
        }
        if (!is_success(std::get<AlpacaMessage>(greeting), "connected")) {
            return unexpected(std::get<AlpacaMessage>(greeting), "the connection greeting");
        }

        source_.enter(MarketDataSourceState::Authenticating, "authenticating with Alpaca");
        {
            std::string frame = source_.credentials_.authentication_message();
            const bool sent = connection_.send(frame);
            wipe(frame);
            if (!sent) {
                return transient("the connection closed while authenticating");
            }
        }
        auto authenticated = await_control(
            source_.options_.authentication_timeout,
            "authentication"
        );
        if (auto* end = std::get_if<SessionEnd>(&authenticated)) {
            return std::move(*end);
        }
        if (!is_success(std::get<AlpacaMessage>(authenticated), "authenticated")) {
            return unexpected(std::get<AlpacaMessage>(authenticated), "authentication");
        }

        const AlpacaSubscription& subscription = source_.normalizer_.subscription();
        source_.enter(
            MarketDataSourceState::Subscribing,
            "subscribing to IEX trades and quotes for " +
                std::to_string(subscription.instruments().size()) + " symbols"
        );
        if (!connection_.send(subscription.subscribe_message())) {
            return transient("the connection closed while subscribing");
        }
        auto acknowledged = await_control(
            source_.options_.subscription_timeout,
            "the subscription acknowledgement"
        );
        if (auto* end = std::get_if<SessionEnd>(&acknowledged)) {
            return std::move(*end);
        }
        const auto* subscribed =
            std::get_if<AlpacaSubscriptionMessage>(&std::get<AlpacaMessage>(acknowledged));
        if (subscribed == nullptr) {
            return unexpected(std::get<AlpacaMessage>(acknowledged), "the subscription");
        }
        if (auto mismatch = subscription.acknowledgement_mismatch(*subscribed)) {
            return fatal(std::move(*mismatch));
        }

        // The run's first sequence, declared to the still-suspended market
        // state: a later start than the last event applied is a jump no quote
        // is executable across (ADR 0020).
        const market_data::BegunRun begun = source_.sequences_.begin_run();
        if (const auto* unavailable = std::get_if<market_data::IngestSequencesUnavailable>(&begun)) {
            return sequences_unavailable(*unavailable);
        }
        const auto& run = std::get<market_data::PublicationRun>(begun);
        if (auto refused = source_.feed_.market_state().continue_at(run.first)) {
            return fatal("the market state refused the stream's next sequence: " + refused->message);
        }
        // Everything this session numbers belongs to this run, and is
        // published, and its quotes served, only while it lasts.
        run_ = run.id;
        source_.publishing_run_.store(run.id);

        live_ = true;
        source_.enter(MarketDataSourceState::Live, std::nullopt);
        return stream();
    }

private:
    using Awaited = std::variant<AlpacaMessage, SessionEnd>;

    // Reads until one control message arrives, the timeout passes, the
    // connection closes, or the source stops.
    Awaited await_control(Clock::duration timeout, std::string_view awaited) {
        const Clock::time_point deadline = source_.clock_.now() + timeout;
        while (true) {
            if (source_.stopping()) {
                return stopped();
            }
            TransportRead read = connection_.read();
            if (source_.stopping()) {
                return stopped();
            }
            if (std::holds_alternative<TransportTimeout>(read)) {
                if (source_.clock_.now() >= deadline) {
                    return transient(
                        "timed out after " + seconds_text(timeout) + " waiting for " +
                        std::string(awaited)
                    );
                }
                continue;
            }
            if (const auto* closed = std::get_if<TransportClosed>(&read)) {
                return transient(
                    "the connection closed while waiting for " + std::string(awaited) +
                    ": " + closed->reason
                );
            }
            if (std::holds_alternative<TransportBinary>(read)) {
                return binary_frame();
            }

            AlpacaDecodeResult decoded =
                decode_alpaca_frame(std::get<TransportText>(read).payload);
            if (const auto* error = std::get_if<AlpacaDecodeError>(&decoded)) {
                return fatal("Alpaca sent a frame Chronos cannot decode: " + error->message);
            }
            auto& messages = std::get<std::vector<AlpacaMessage>>(decoded);
            if (messages.empty()) {
                continue;
            }
            if (messages.size() != 1 || !is_control_message(messages.front())) {
                return fatal(
                    "Alpaca sent market data or several messages while Chronos was "
                    "waiting for " + std::string(awaited) +
                    "; control messages must arrive alone and before data"
                );
            }
            if (const auto* error = std::get_if<AlpacaErrorMessage>(&messages.front())) {
                return error_end(*error);
            }
            return std::move(messages.front());
        }
    }

    SessionEnd stream() {
        last_clock_notice_ = source_.clock_.now();
        while (true) {
            if (source_.stopping()) {
                return stopped();
            }
            // Before every read, after a frame, an empty frame, or an idle
            // timeout alike: a run that ended, even with nothing to
            // publish, ends the session.
            if (auto refused = source_.sequences_.publication_refusal(run_)) {
                return sequences_unavailable(*refused);
            }
            TransportRead read = connection_.read();
            // Receipt time is taken before any parsing.
            const Timestamp received = source_.receive_clock_.now();
            if (source_.stopping()) {
                return stopped();
            }
            if (std::holds_alternative<TransportTimeout>(read)) {
                if (auto end = advance_clock(received)) {
                    return std::move(*end);
                }
                continue;
            }
            if (const auto* closed = std::get_if<TransportClosed>(&read)) {
                return transient("the connection closed: " + closed->reason);
            }
            if (std::holds_alternative<TransportBinary>(read)) {
                return binary_frame();
            }
            if (auto end = apply_frame(std::get<TransportText>(read).payload, received)) {
                return std::move(*end);
            }
        }
    }

    std::optional<SessionEnd> apply_frame(const std::string& frame, Timestamp received) {
        AlpacaDecodeResult decoded = decode_alpaca_frame(frame);
        if (const auto* error = std::get_if<AlpacaDecodeError>(&decoded)) {
            return fatal("Alpaca sent a frame Chronos cannot decode: " + error->message);
        }
        auto& messages = std::get<std::vector<AlpacaMessage>>(decoded);
        std::vector<AlpacaDataMessage> data;
        data.reserve(messages.size());
        for (AlpacaMessage& message : messages) {
            if (is_control_message(message)) {
                if (messages.size() != 1) {
                    return fatal(
                        "Alpaca sent a control message together with other messages"
                    );
                }
                if (const auto* error = std::get_if<AlpacaErrorMessage>(&message)) {
                    return error_end(*error);
                }
                return fatal(
                    "Alpaca sent an unexpected " + std::string(control_kind(message)) +
                    " message while live"
                );
            }
            if (auto* quote = std::get_if<AlpacaQuote>(&message)) {
                data.emplace_back(std::move(*quote));
            } else if (auto* trade = std::get_if<AlpacaTrade>(&message)) {
                data.emplace_back(std::move(*trade));
            }
            // Corrections, cancels, and other documented types are not
            // applied; execution and valuation use quotes only.
        }

        if (auto end = advance_clock(received)) {
            return end;
        }
        if (data.empty()) {
            return std::nullopt;
        }
        // The whole frame is numbered at once, or none of it.
        const market_data::IssuedSequence issued = source_.sequences_.issue(run_, data.size());
        if (const auto* unavailable = std::get_if<market_data::IngestSequencesUnavailable>(&issued)) {
            return sequences_unavailable(*unavailable);
        }
        AlpacaNormalizationResult normalized =
            source_.normalizer_.normalize(data, received, std::get<std::uint64_t>(issued));
        if (const auto* error = std::get_if<AlpacaNormalizationError>(&normalized)) {
            return fatal(
                "Alpaca sent data that violates the Chronos market-data contract: " +
                validation_details(*error)
            );
        }
        for (const ValidatedMarketEvent& event :
             std::get<std::vector<ValidatedMarketEvent>>(normalized)) {
            market_data::FeedPublication published = source_.feed_.publish(event, source_.sequences_, run_);
            if (published.refused) {
                // Not applied. The rest of the frame is never published; the
                // next run begins after it.
                return sequences_unavailable(*published.refused);
            }
            if (published.failed) {
                return fatal(std::move(*published.failed));
            }
        }
        return std::nullopt;
    }

    SessionEnd sequences_unavailable(const market_data::IngestSequencesUnavailable& unavailable) const {
        return unavailable.permanent ? fatal(sequence_diagnostic(unavailable))
                                     : transient(sequence_diagnostic(unavailable));
    }

    // Moves market time to the local receive clock, so quotes age while
    // the feed is quiet, and occasionally tells subscribers.
    std::optional<SessionEnd> advance_clock(Timestamp received) {
        if (auto error = source_.feed_.market_state().advance_time(received)) {
            return fatal("market time could not advance: " + error->message);
        }
        const Clock::time_point now = source_.clock_.now();
        if (now - last_clock_notice_ >= source_.options_.clock_notice_interval) {
            last_clock_notice_ = now;
            source_.feed_.notifier().publish();
        }
        return std::nullopt;
    }

    SessionEnd error_end(const AlpacaErrorMessage& error) const {
        const std::string text = alpaca_error_text(error);
        switch (error.code) {
            case 402:
                return fatal(
                    text + ": the credentials were rejected; check " +
                    std::string(kAlpacaKeyIdVariable) + " and " +
                    std::string(kAlpacaSecretKeyVariable) + ", then restart the server"
                );
            case 409:
                return fatal(text + ": the Alpaca account's data plan does not include this feed");
            case 405:
                return fatal(text + ": the subscription exceeds the plan's symbol limit");
            case 410:
                return fatal(text + ": the feed does not offer the requested channels");
            case 400:
            case 401:
            case 403:
                return fatal(text + ": Alpaca did not accept a Chronos request");
            case 404:
            case 406:
            case 407:
            case 500:
                return transient(text);
            default:
                return fatal(text + ": the error code is not documented");
        }
    }

    SessionEnd unexpected(const AlpacaMessage& message, std::string_view step) const {
        std::string received = std::string(control_kind(message));
        if (const auto* success = std::get_if<AlpacaSuccess>(&message)) {
            received += " \"" + printable_excerpt(success->message) + "\"";
        }
        return fatal(
            "Alpaca sent an unexpected " + received + " message during " + std::string(step)
        );
    }

    static bool is_success(const AlpacaMessage& message, std::string_view text) {
        const auto* success = std::get_if<AlpacaSuccess>(&message);
        return success != nullptr && success->message == text;
    }

    SessionEnd binary_frame() const {
        return fatal("Alpaca sent a binary frame, which Chronos never requests");
    }

    SessionEnd stopped() const {
        return SessionEnd{.ending = Ending::Stopped, .diagnostic = {}, .reached_live = live_};
    }
    SessionEnd transient(std::string diagnostic) const {
        return SessionEnd{
            .ending = Ending::Transient,
            .diagnostic = std::move(diagnostic),
            .reached_live = live_,
        };
    }
    SessionEnd fatal(std::string diagnostic) const {
        return SessionEnd{
            .ending = Ending::Fatal,
            .diagnostic = std::move(diagnostic),
            .reached_live = live_,
        };
    }

    AlpacaLiveSource& source_;
    AlpacaConnection& connection_;
    bool live_{false};
    // The publication run this session began; set before it is live.
    std::uint64_t run_{0};
    Clock::time_point last_clock_notice_{};
};

AlpacaLiveSource::AlpacaLiveSource(
    std::unique_ptr<AlpacaTransport> transport,
    AlpacaCredentials credentials,
    AlpacaSubscription subscription,
    market_data::MarketStateFeed& feed,
    market_data::LiveIngestSequences& sequences,
    const market_data::MonotonicClock& clock,
    const market_data::MonotonicUtcClock& receive_clock,
    market_data::BackoffJitter jitter,
    AlpacaLiveOptions options,
    DiagnosticLog log
)
    : transport_(std::move(transport)),
      credentials_(std::move(credentials)),
      description_(description(subscription)),
      feed_(feed),
      sequences_(sequences),
      clock_(clock),
      receive_clock_(receive_clock),
      backoff_(options.backoff, std::move(jitter)),
      options_(options),
      log_(std::move(log)),
      normalizer_(
          feed.market_state().status().stream,
          std::move(subscription)
      ) {
    // No quote is current until the first subscription is live.
    feed_.market_state().suspend();
    feed_.market_state().set_quote_authority(&authority_);
    worker_ = std::thread([this] { run(); });
}

AlpacaLiveSource::~AlpacaLiveSource() {
    stop();
    feed_.market_state().set_quote_authority(nullptr);
}

std::optional<std::string> AlpacaLiveSource::RunAuthority::quote_refusal() const {
    const std::uint64_t run = source_.publishing_run_.load();
    if (run == 0) {
        return "no live session is publishing";
    }
    if (auto refused = source_.sequences_.publication_refusal(run)) {
        return std::move(refused->diagnostic);
    }
    return std::nullopt;
}

market_data::MarketDataSourceDescription AlpacaLiveSource::description(
    const AlpacaSubscription& subscription
) {
    return market_data::MarketDataSourceDescription{
        .mode = market_data::MarketDataMode::Live,
        .provider = "Alpaca",
        .feed = "IEX",
        .label = "Alpaca IEX",
        .coverage_notice =
            "IEX only: quotes and trades from the Investors Exchange alone. "
            "This is not consolidated SIP data, not the NBBO, and not every "
            "US exchange.",
        .coverage_label = "IEX only, not SIP or NBBO",
        .instruments = subscription.instruments(),
    };
}

market_data::MarketDataSourceStatus AlpacaLiveSource::status() const {
    const std::lock_guard lock(mutex_);
    return market_data::MarketDataSourceStatus{
        .description = description_,
        .state = state_,
        .events_applied = feed_.events_applied(),
        .replay_speed = std::nullopt,
        .diagnostic = diagnostic_,
        .recording_sha256 = std::nullopt,
    };
}

market_data::ReplayCommandResult AlpacaLiveSource::control_replay(
    market_data::ReplayCommand
) {
    const std::lock_guard lock(mutex_);
    return market_data::ReplayCommandError{
        .code = market_data::ReplayCommandErrorCode::NotReplay,
        .state = state_,
        .message = "the market-data source is live, not a replay",
    };
}

void AlpacaLiveSource::stop() noexcept {
    {
        const std::lock_guard lock(mutex_);
        stop_requested_ = true;
    }
    changed_.notify_all();
    transport_->interrupt();
    // Safe from several threads at once: later callers wait for the join.
    std::call_once(joined_, [this] {
        if (worker_.joinable()) {
            worker_.join();
        }
    });
}

void AlpacaLiveSource::run() noexcept {
    try {
        operate();
    } catch (const std::exception& error) {
        finish(
            MarketDataSourceState::Failed,
            std::string("the live source stopped unexpectedly: ") + error.what()
        );
    } catch (...) {
        finish(MarketDataSourceState::Failed, "the live source stopped unexpectedly");
    }
}

void AlpacaLiveSource::operate() {
    while (true) {
        SessionEnd end = run_session();
        switch (end.ending) {
            case Ending::Stopped:
                finish(MarketDataSourceState::Stopped, std::nullopt);
                return;
            case Ending::Fatal:
                finish(MarketDataSourceState::Failed, std::move(end.diagnostic));
                return;
            case Ending::Transient:
                break;
        }
        if (end.reached_live) {
            backoff_.reset();
        }
        const Clock::duration delay = backoff_.next_delay();
        enter(
            MarketDataSourceState::Reconnecting,
            end.diagnostic + "; reconnecting in " + seconds_text(delay)
        );
        if (!wait_for_backoff(delay)) {
            finish(MarketDataSourceState::Stopped, std::nullopt);
            return;
        }
    }
}

AlpacaLiveSource::SessionEnd AlpacaLiveSource::run_session() {
    // Without the stream's writer lease and reserved sequences, nothing could
    // be published, so no upstream connection is opened either.
    if (auto unavailable = sequences_.readiness()) {
        return SessionEnd{
            .ending = unavailable->permanent ? Ending::Fatal : Ending::Transient,
            .diagnostic = sequence_diagnostic(*unavailable),
            .reached_live = false,
        };
    }
    enter(MarketDataSourceState::Connecting, "connecting to Alpaca IEX");
    TransportConnectResult connected = transport_->connect();
    if (stopping()) {
        return SessionEnd{.ending = Ending::Stopped, .diagnostic = {}, .reached_live = false};
    }
    if (const auto* failure = std::get_if<TransportConnectFailure>(&connected)) {
        return SessionEnd{
            .ending = Ending::Transient,
            .diagnostic = "could not connect to Alpaca: " + failure->reason,
            .reached_live = false,
        };
    }
    // The connection closes when this scope ends, before any reconnect, so
    // at most one upstream WebSocket exists at a time.
    const auto connection = std::get<std::unique_ptr<AlpacaConnection>>(std::move(connected));
    // However the session ends, its run no longer vouches for any quote.
    const struct EndRun {
        std::atomic<std::uint64_t>& run;
        ~EndRun() { run.store(0); }
    } end_run{publishing_run_};
    Session session(*this, *connection);
    return session.run();
}

bool AlpacaLiveSource::wait_for_backoff(Clock::duration delay) {
    std::unique_lock lock(mutex_);
    const Clock::time_point deadline = clock_.now() + delay;
    clock_.wait_until(lock, changed_, deadline, [this] { return stop_requested_.load(); });
    return !stop_requested_.load();
}

void AlpacaLiveSource::enter(
    MarketDataSourceState state,
    std::optional<std::string> diagnostic
) {
    // Availability changes before the status that explains it, so no reader
    // sees a live status with a suspended market or the reverse for long.
    if (state == MarketDataSourceState::Live) {
        feed_.market_state().resume();
    } else {
        feed_.market_state().suspend();
    }
    std::optional<std::string> safe = diagnostic
        ? std::optional<std::string>(credentials_.redact(std::move(*diagnostic)))
        : std::nullopt;
    {
        const std::lock_guard lock(mutex_);
        state_ = state;
        diagnostic_ = safe;
    }
    changed_.notify_all();
    feed_.notifier().publish();
    log(
        "Alpaca IEX " + std::string(market_data::state_name(state)) +
        (safe ? ": " + *safe : std::string())
    );
}

void AlpacaLiveSource::finish(
    MarketDataSourceState state,
    std::optional<std::string> diagnostic
) {
    feed_.market_state().suspend();
    std::optional<std::string> safe = diagnostic
        ? std::optional<std::string>(credentials_.redact(std::move(*diagnostic)))
        : std::nullopt;
    {
        const std::lock_guard lock(mutex_);
        state_ = state;
        diagnostic_ = safe;
    }
    changed_.notify_all();
    feed_.notifier().publish();
    if (state == MarketDataSourceState::Failed) {
        log(
            "Alpaca IEX failed after " + std::to_string(feed_.events_applied()) +
            " events: " + safe.value_or("unknown failure")
        );
    } else {
        log("Alpaca IEX stopped");
    }
}

void AlpacaLiveSource::log(const std::string& line) const {
    if (log_) {
        log_(credentials_.redact(line));
    }
}

}  // namespace chronos::alpaca_adapter
