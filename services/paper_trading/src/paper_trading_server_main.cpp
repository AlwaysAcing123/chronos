#include "chronos/market_data/leased_ingest_sequences.hpp"
#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_data_source.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/market_data/monotonic_clock.hpp"
#include "chronos/market_data/monotonic_utc_clock.hpp"
#include "chronos/market_state.hpp"
#include "chronos/paper_trading/deployment_inference.hpp"
#include "chronos/paper_trading/deployment_runtime.hpp"
#include "chronos/paper_trading/paper_trading_http_api.hpp"
#include "chronos/paper_trading/paper_trading_http_contract.hpp"
#include "chronos/paper_trading/paper_trading_http_server.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"
#include "chronos/paper_trading/server_arguments.hpp"
#include "chronos/paper_trading/working_order_matcher.hpp"
#include "chronos/postgresql_live_stream_sequences.hpp"

#if defined(CHRONOS_PAPER_TRADING_SERVER_WITH_DATABENTO_REPLAY)
#include "chronos/databento/equities_mini_mbp1_event_reader.hpp"
#include "chronos/market_data/paced_replay_source.hpp"
#endif

#if defined(CHRONOS_PAPER_TRADING_SERVER_WITH_ALPACA_LIVE)
#include "chronos/alpaca/alpaca_credentials.hpp"
#include "chronos/alpaca/alpaca_live_source.hpp"
#include "chronos/alpaca/alpaca_subscription.hpp"
#include "chronos/alpaca/alpaca_websocket_transport.hpp"
#include "chronos/market_data/reconnect_backoff.hpp"
#endif

#include <pthread.h>
#include <signal.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace market_data = chronos::market_data;
namespace paper_trading = chronos::paper_trading;

constexpr std::string_view kDatabaseUrlVariable = "CHRONOS_POSTGRES_URL";

constexpr paper_trading::ServerCapabilities kCapabilities{
#if defined(CHRONOS_PAPER_TRADING_SERVER_WITH_DATABENTO_REPLAY)
    .databento_replay = true,
#else
    .databento_replay = false,
#endif
#if defined(CHRONOS_PAPER_TRADING_SERVER_WITH_ALPACA_LIVE)
    .alpaca_live = true,
#else
    .alpaca_live = false,
#endif
};

// Source, HTTP, and startup threads all log; each line is one write, so
// lines never interleave.
void log_line(std::string_view line) {
    static std::mutex mutex;
    std::string text(line);
    text += '\n';
    const std::lock_guard lock(mutex);
    std::cerr << text << std::flush;
}

// A live stream's ingest sequences, held under its durable writer lease
// (ADR 0020); absent for a replay or an empty market, whose identities are
// their own.
struct LiveStreamSequences {
    chronos::PostgresIngestSequenceLeaseStore store;
    market_data::LeasedIngestSequences sequences;

    LiveStreamSequences(
        const paper_trading::ServerArguments& arguments,
        std::string_view database_url,
        const market_data::MonotonicClock& clock
    )
        : store(std::string(database_url)),
          sequences(
              chronos::MarketDataStreamId(arguments.stream_id),
              store,
              market_data::this_process_stream_writer(),
              clock,
              market_data::LeasedIngestSequencesOptions{
                  .adopt_history = arguments.adopt_live_stream_history,
              },
              log_line
          ) {}
};

// The server's single market-data source, feeding `feed` on its own thread.
std::unique_ptr<market_data::MarketDataSource> make_source(
    const paper_trading::ServerArguments& arguments,
    market_data::MarketStateFeed& feed,
    market_data::LiveIngestSequences* live_sequences,
    const market_data::MonotonicClock& clock,
    const market_data::MonotonicUtcClock& receive_clock
) {
#if defined(CHRONOS_PAPER_TRADING_SERVER_WITH_ALPACA_LIVE)
    if (arguments.alpaca_iex) {
        namespace alpaca = chronos::alpaca_adapter;
        // Credentials come only from the environment, and an error names
        // the missing variable, never a value.
        auto credentials = alpaca::AlpacaCredentials::from_environment(
            alpaca::AlpacaCredentials::process_environment()
        );
        if (const auto* error =
                std::get_if<alpaca::AlpacaCredentialsError>(&credentials)) {
            throw paper_trading::ServerUsageError(error->message);
        }
        auto subscription = alpaca::AlpacaSubscription::create(arguments.instruments);
        if (const auto* error =
                std::get_if<alpaca::AlpacaSubscriptionError>(&subscription)) {
            throw paper_trading::ServerUsageError("--instrument: " + error->message);
        }
        return std::make_unique<alpaca::AlpacaLiveSource>(
            alpaca::make_alpaca_iex_transport(),
            std::get<alpaca::AlpacaCredentials>(std::move(credentials)),
            std::get<alpaca::AlpacaSubscription>(std::move(subscription)),
            feed,
            *live_sequences,
            clock,
            receive_clock,
            market_data::random_backoff_jitter(),
            alpaca::AlpacaLiveOptions{},
            log_line
        );
    }
#else
    static_cast<void>(receive_clock);
    static_cast<void>(live_sequences);
#endif
#if defined(CHRONOS_PAPER_TRADING_SERVER_WITH_DATABENTO_REPLAY)
    if (arguments.replay) {
        namespace adapter = chronos::databento_adapter;
        auto opened = adapter::EquitiesMiniMbp1EventReader::open(
            arguments.replay->recording,
            chronos::MarketDataStreamId(arguments.stream_id)
        );
        if (const auto* error =
                std::get_if<adapter::EquitiesMiniMbp1FileError>(&opened)) {
            throw std::runtime_error(
                "could not open recording: " + error->message
            );
        }
        return std::make_unique<market_data::PacedReplaySource>(
            std::get<std::unique_ptr<adapter::EquitiesMiniMbp1EventReader>>(
                std::move(opened)
            ),
            feed,
            clock,
            market_data::MarketDataSourceDescription{
                .mode = market_data::MarketDataMode::HistoricalReplay,
                .provider = "Databento",
                .feed = "EQUS.MINI MBP-1",
                .label = "Databento EQUS.MINI MBP-1 replay",
                .coverage_notice = std::nullopt,
                .coverage_label = std::nullopt,
                .instruments = arguments.instruments,
            },
            market_data::PacedReplayOptions{
                .speed = arguments.replay->speed,
                .start_immediately = arguments.replay->start_immediately,
            },
            log_line
        );
    }
#else
    static_cast<void>(feed);
    static_cast<void>(clock);
#endif
    return std::make_unique<market_data::NoMarketDataSource>(
        arguments.instruments
    );
}

int run(const paper_trading::ServerArguments& arguments) {
    const char* database_url = std::getenv(kDatabaseUrlVariable.data());
    if (database_url == nullptr || std::string_view(database_url).empty()) {
        throw paper_trading::ServerUsageError(
            std::string(kDatabaseUrlVariable) + " must be set"
        );
    }

    // Block termination signals before any thread starts, so every server
    // thread inherits the mask and only the waiter below receives them.
    sigset_t termination_signals;
    sigemptyset(&termination_signals);
    sigaddset(&termination_signals, SIGINT);
    sigaddset(&termination_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &termination_signals, nullptr);

    // Model deployments only when asked for, with every path checked before
    // anything starts. A write to an inference worker that has exited must
    // fail with EPIPE rather than end the server.
    std::unique_ptr<paper_trading::InferenceProvider> inference;
    if (arguments.deployments) {
        const auto& paths = *arguments.deployments;
        if (!std::filesystem::is_regular_file(paths.python)) {
            throw paper_trading::ServerUsageError("--research-python is not a file: " + paths.python.string());
        }
        if (!std::filesystem::is_directory(paths.python_root / "chronos_research")) {
            throw paper_trading::ServerUsageError(
                "--research-python-root has no chronos_research package: " + paths.python_root.string()
            );
        }
        if (!std::filesystem::is_directory(paths.artifacts / "experiments")) {
            throw paper_trading::ServerUsageError(
                "--research-artifacts has no experiments directory: " + paths.artifacts.string()
            );
        }
        signal(SIGPIPE, SIG_IGN);
        inference = std::make_unique<paper_trading::PythonInferenceProvider>(paper_trading::PythonWorkerCommand{
            .python = std::filesystem::absolute(paths.python),
            .python_root = std::filesystem::absolute(paths.python_root),
            .artifacts = std::filesystem::absolute(paths.artifacts),
        });
    }

    // Declared in dependency order, so destruction runs in reverse: the
    // HTTP server stops first, and the source's thread is joined before the
    // matcher, market state, and notifier it feeds are destroyed.
    const market_data::SteadyClock clock;
    chronos::MarketState market_state{
        chronos::MarketDataStreamId(arguments.stream_id),
    };
    market_data::MarketDataNotifier notifier(
        clock,
        market_data::MarketDataNotifier::Options{}
    );
    // Working orders are matched on the source's thread with a PostgreSQL
    // connection of their own, so matching never waits for the API's.
    // A match waits at most a second for an account another session holds,
    // then retries, so a held lock cannot stop shutdown.
    paper_trading::WorkingOrderMatcher matcher(
        std::make_unique<chronos::PostgresPaperOrderRepository>(
            database_url,
            std::chrono::milliseconds(1'000)
        ),
        paper_trading::WorkingOrderMatcherOptions{},
        [&notifier] { notifier.publish_activity(); },
        log_line
    );
    // The feed's observer: the matcher first, then deployments' features
    // and hand-offs, which never wait (ADR 0018).
    paper_trading::DeploymentHub deployment_hub(&matcher);
    // Applying and matching an event excludes submissions, which hold the
    // gate from reading their quote until they commit.
    market_data::MarketEventGate market_gate;
    market_data::MarketStateFeed feed(
        market_state,
        notifier,
        0,
        &deployment_hub,
        &market_gate
    );
    // The only read of the system clock: a live feed's receive times are
    // this instant plus monotonic elapsed time.
    const market_data::MonotonicUtcClock receive_clock(
        clock,
        market_data::system_utc_now()
    );
    // A live stream's sequences outlive its source: the source is destroyed,
    // and its thread joined, before the lease is released.
    std::unique_ptr<LiveStreamSequences> live_sequences;
    if (arguments.alpaca_iex) {
        live_sequences = std::make_unique<LiveStreamSequences>(arguments, database_url, clock);
        live_sequences->sequences.start();
    }
    const std::unique_ptr<market_data::MarketDataSource> source = make_source(
        arguments,
        feed,
        live_sequences ? &live_sequences->sequences : nullptr,
        clock,
        receive_clock
    );
    // Destroyed before the source, whose destructor joins its thread: if
    // startup fails from here on, a match retrying a lost connection on that
    // thread is ended first, so the join cannot wait for the database.
    const struct StopMatchingFirst {
        paper_trading::WorkingOrderMatcher& matcher;
        ~StopMatchingFirst() { matcher.stop(); }
    } stop_matching_first{matcher};
    const market_data::MarketDataSourceStatus status = source->status();
    log_line(
        "market stream " + arguments.stream_id + ": " +
        status.description.label + " (" +
        std::string(market_data::state_name(status.state)) + ")"
    );
    if (status.description.coverage_notice) {
        log_line("market data coverage: " + *status.description.coverage_notice);
    }

    // A request waits at most five seconds for a locked account, then
    // reports persistence as unavailable rather than holding the API.
    paper_trading::PaperTradingService service(
        std::make_unique<chronos::PostgresPaperOrderRepository>(
            database_url,
            std::chrono::milliseconds(5'000)
        ),
        market_state,
        paper_trading::PaperTradingServiceConfig{
            .tradeable_instruments = arguments.instruments,
            .account_risk_limits = arguments.risk_limits,
        },
        [&notifier] { notifier.publish_activity(); },
        &market_gate
    );
    service.set_trading_status_view(&deployment_hub);
    // Always present, so a server without the opt-in still stops and lists
    // deployments; only with it can one be created or started.
    paper_trading::DeploymentManager deployments(
        service,
        deployment_hub,
        std::move(inference),
        paper_trading::DeploymentRuntimeOptions{
            .stream = chronos::MarketDataStreamId(arguments.stream_id),
            .tradeable_instruments = arguments.instruments,
            .account_risk_limits = arguments.risk_limits,
            .source_status = [&source] { return source->status(); },
        },
        log_line
    );
    // Nothing resumes on its own: whatever was running is now stopped.
    deployments.recover();
    log_line(
        deployments.enabled()
            ? "model deployments: enabled; each runs only after an explicit Start"
            : "model deployments: disabled (start with --model-deployments to enable)"
    );
    paper_trading::PaperTradingHttpApi api(
        service,
        *source,
        notifier,
        log_line,
        &matcher,
        &deployments
    );
    paper_trading::PaperTradingHttpServer server(api, arguments.http, log_line);
    const int port = server.bind();
    log_line(
        "PAPER TRADING ONLY: listening on http://" + arguments.http.host + ":" +
        std::to_string(port) + " (no authentication; not production-secure)"
    );

    std::thread signal_waiter([&] {
        int received = 0;
        sigwait(&termination_signals, &received);
        server.stop();
    });
    std::exception_ptr listen_failure;
    try {
        server.listen();
    } catch (...) {
        listen_failure = std::current_exception();
    }
    // Wakes the waiter when listening ended for a reason other than a signal;
    // the thread must be joined before it is destroyed.
    pthread_kill(signal_waiter.native_handle(), SIGTERM);
    signal_waiter.join();
    // Deployments stop first (server_stopped), ending their workers.
    deployments.shutdown();
    // A match waiting for PostgreSQL would hold the source's thread, so end
    // it before joining that thread.
    matcher.stop();
    source->stop();
    // Nothing can publish any more, so the stream may pass to the next
    // server at once.
    if (live_sequences) {
        live_sequences->sequences.close();
    }
    if (listen_failure) {
        std::rethrow_exception(listen_failure);
    }
    log_line("stopped");
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const std::vector<std::string_view> arguments(argv + 1, argv + argc);
        return run(paper_trading::parse_server_arguments(arguments, kCapabilities));
    } catch (const paper_trading::ServerUsageError& error) {
        if (error.what()[0] != '\0') {
            std::cerr << "error: " << error.what() << "\n\n";
        }
        std::cerr << paper_trading::server_usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
