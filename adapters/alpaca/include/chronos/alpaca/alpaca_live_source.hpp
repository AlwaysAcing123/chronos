#pragma once

#include "chronos/alpaca/alpaca_credentials.hpp"
#include "chronos/alpaca/alpaca_normalizer.hpp"
#include "chronos/alpaca/alpaca_subscription.hpp"
#include "chronos/alpaca/alpaca_transport.hpp"
#include "chronos/market_data/live_ingest_sequences.hpp"
#include "chronos/market_data/market_data_source.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/market_data/monotonic_clock.hpp"
#include "chronos/market_data/monotonic_utc_clock.hpp"
#include "chronos/market_data/reconnect_backoff.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace chronos::alpaca_adapter {

struct AlpacaLiveOptions {
    // After the WebSocket opens, for Alpaca's "connected" greeting.
    market_data::MonotonicClock::duration greeting_timeout{std::chrono::seconds(10)};
    // After sending credentials, for "authenticated".
    market_data::MonotonicClock::duration authentication_timeout{std::chrono::seconds(10)};
    // After subscribing, for the subscription acknowledgement.
    market_data::MonotonicClock::duration subscription_timeout{std::chrono::seconds(10)};
    market_data::ReconnectBackoffPolicy backoff{};
    // Least time between change notices sent only because market time
    // advanced without an event; every event still notifies at once.
    market_data::MonotonicClock::duration clock_notice_interval{std::chrono::seconds(1)};
};

// Alpaca's free IEX stock stream as the server's live market-data source.
//
// It owns exactly one upstream WebSocket at a time and moves through an
// explicit lifecycle on its own thread:
//
//   Connecting -> (greeting) -> Authenticating -> Subscribing -> Live
//        ^                                                         |
//        +---------------- Reconnecting (backoff) <----------------+
//
// Each handshake step has its own timeout. A lost connection, a timeout,
// and Alpaca's transient errors (auth timeout, connection limit, slow
// client, internal error) reconnect after a bounded exponential backoff.
// Rejected credentials, an insufficient subscription, a subscription that
// does not match the request, an unknown message type, and any message
// that violates the market-data contract fail the source permanently, so
// malformed data is never hidden by reconnecting forever.
//
// The market state is suspended in every state but Live, so no quote is
// executable while the feed is not current. Reaching Live resumes it, and
// each instrument then needs a quote received on the new connection. While
// Live, every read, including an idle poll, advances market time to the
// local receive clock, so a silent feed's quotes age and become stale.
//
// Ingest sequences come from the stream's issuer (ADR 0020), which holds the
// stream's durable writer lease. They continue across reconnects and across
// server restarts, never repeating or moving back. Without the lease the
// source opens no connection and publishes nothing; each run declares where
// it begins to the market state while it is still suspended, and every
// event is published only if its run is still current at that instant.
// Each session publishes in a run of its own. The source vouches for the
// market state's quotes only while that run is current, so a lapse ends
// their use at once, even if no frame arrives; the session then ends at its
// next read, and only a new session, with a new run and fresh quotes,
// makes quotes executable again.
//
// Credentials go only into the authentication frame. Every diagnostic and
// log line is passed through the credentials' redaction, and none contains
// frame content beyond Alpaca's short error text.
class AlpacaLiveSource final : public market_data::MarketDataSource {
public:
    using DiagnosticLog = std::function<void(std::string_view line)>;

    AlpacaLiveSource(
        std::unique_ptr<AlpacaTransport> transport,
        AlpacaCredentials credentials,
        AlpacaSubscription subscription,
        market_data::MarketStateFeed& feed,
        market_data::LiveIngestSequences& sequences,
        const market_data::MonotonicClock& clock,
        const market_data::MonotonicUtcClock& receive_clock,
        market_data::BackoffJitter jitter,
        AlpacaLiveOptions options = {},
        DiagnosticLog log = {}
    );
    ~AlpacaLiveSource() override;

    AlpacaLiveSource(const AlpacaLiveSource&) = delete;
    AlpacaLiveSource& operator=(const AlpacaLiveSource&) = delete;
    AlpacaLiveSource(AlpacaLiveSource&&) = delete;
    AlpacaLiveSource& operator=(AlpacaLiveSource&&) = delete;

    [[nodiscard]] market_data::MarketDataSourceStatus status() const override;
    [[nodiscard]] market_data::ReplayCommandResult control_replay(
        market_data::ReplayCommand command
    ) override;
    void stop() noexcept override;

    // How the source describes itself: "Alpaca IEX", with the notice that
    // IEX is one exchange, not consolidated SIP data or the NBBO.
    [[nodiscard]] static market_data::MarketDataSourceDescription description(
        const AlpacaSubscription& subscription
    );

private:
    enum class Ending {
        Stopped,
        Transient,
        Fatal,
    };

    struct SessionEnd {
        Ending ending;
        std::string diagnostic;
        bool reached_live{false};
    };

    class Session;

    // Answers for the market state's quotes: current only while the live
    // session's run is.
    class RunAuthority final : public MarketQuoteAuthority {
    public:
        explicit RunAuthority(const AlpacaLiveSource& source) : source_(source) {}
        [[nodiscard]] std::optional<std::string> quote_refusal() const override;

    private:
        const AlpacaLiveSource& source_;
    };

    void run() noexcept;
    void operate();
    [[nodiscard]] SessionEnd run_session();
    [[nodiscard]] bool wait_for_backoff(market_data::MonotonicClock::duration delay);
    void enter(
        market_data::MarketDataSourceState state,
        std::optional<std::string> diagnostic
    );
    void finish(
        market_data::MarketDataSourceState state,
        std::optional<std::string> diagnostic
    );
    void log(const std::string& line) const;
    [[nodiscard]] bool stopping() const noexcept { return stop_requested_.load(); }

    std::unique_ptr<AlpacaTransport> transport_;
    const AlpacaCredentials credentials_;
    const market_data::MarketDataSourceDescription description_;
    market_data::MarketStateFeed& feed_;
    market_data::LiveIngestSequences& sequences_;
    const market_data::MonotonicClock& clock_;
    const market_data::MonotonicUtcClock& receive_clock_;
    market_data::ReconnectBackoff backoff_;
    const AlpacaLiveOptions options_;
    DiagnosticLog log_;
    AlpacaNormalizer normalizer_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    market_data::MarketDataSourceState state_{market_data::MarketDataSourceState::Connecting};
    std::optional<std::string> diagnostic_;
    std::atomic<bool> stop_requested_{false};
    // The run the current session publishes in; 0 between sessions. Run IDs
    // start at 1.
    std::atomic<std::uint64_t> publishing_run_{0};
    RunAuthority authority_{*this};

    std::once_flag joined_;
    // Started last, after every member it uses exists.
    std::thread worker_;
};

}  // namespace chronos::alpaca_adapter
