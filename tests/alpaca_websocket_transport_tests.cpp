#include "chronos/alpaca/alpaca_live_source.hpp"
#include "chronos/alpaca/alpaca_websocket_transport.hpp"
#include "market_data_test_support.hpp"
#include "test_support.hpp"

#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::ExecutionQuote;
using chronos::InstrumentId;
using chronos::MarketDataStreamId;
using chronos::MarketState;
using chronos::Price;
using chronos::Timestamp;
using chronos::alpaca_adapter::AlpacaCredentials;
using chronos::alpaca_adapter::AlpacaLiveSource;
using chronos::alpaca_adapter::AlpacaSubscription;
using chronos::alpaca_adapter::make_loopback_websocket_transport;
using chronos::alpaca_adapter::TransportClosed;
using chronos::alpaca_adapter::TransportConnectFailure;
using chronos::alpaca_adapter::TransportText;
using chronos::alpaca_adapter::TransportTimeout;
using chronos::alpaca_adapter::WebSocketTransportTimeouts;
using chronos::market_data::MarketDataNotifier;
using chronos::market_data::MarketDataSourceState;
using chronos::market_data::MarketStateFeed;
using chronos::market_data::MonotonicClock;
using chronos::market_data::MonotonicUtcClock;
using chronos::test::eventually;
using chronos::test::ManualClock;
using namespace std::chrono_literals;

constexpr std::int64_t kReceiptStart = 1'790'602'201'000'000'000;
constexpr std::string_view kFakeKeyId = "FAKEKEYID000000TEST";
constexpr std::string_view kFakeSecret = "fake-secret-value-for-tests-only";

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// A WebSocket server on 127.0.0.1 that speaks Alpaca's handshake once per
// connection, sends one quote, and closes when the test releases it.
class FakeAlpacaServer final {
public:
    FakeAlpacaServer() {
        server_.WebSocket("/v2/iex", [this](const httplib::Request&, httplib::ws::WebSocket& socket) {
            serve(socket);
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        REQUIRE(port_ > 0);
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        REQUIRE(eventually([this] { return server_.is_running(); }));
    }

    ~FakeAlpacaServer() {
        release();
        server_.stop();
        thread_.join();
    }

    FakeAlpacaServer(const FakeAlpacaServer&) = delete;
    FakeAlpacaServer& operator=(const FakeAlpacaServer&) = delete;

    [[nodiscard]] std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(port_) + "/v2/iex";
    }

    // Lets the connection close from the server side.
    void release() {
        const std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] std::vector<std::string> received() const {
        const std::lock_guard lock(mutex_);
        return received_;
    }

private:
    void serve(httplib::ws::WebSocket& socket) {
        socket.send(R"([{"T":"success","msg":"connected"}])");
        std::string message;
        if (socket.read(message) != httplib::ws::Text) {
            return;
        }
        record(message);
        socket.send(R"([{"T":"success","msg":"authenticated"}])");
        if (socket.read(message) != httplib::ws::Text) {
            return;
        }
        record(message);
        socket.send(R"([{"T":"subscription","trades":["AAPL"],"quotes":["AAPL"],"bars":[]}])");
        socket.send(
            R"([{"T":"q","S":"AAPL","bx":"V","bp":227.34,"bs":300,"ax":"V","ap":227.36,)"
            R"("as":100,"c":["R"],"z":"C","t":"2026-09-28T13:30:00.1Z"}])"
        );
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return released_; });
        }
        socket.close();
    }

    void record(const std::string& message) {
        const std::lock_guard lock(mutex_);
        received_.push_back(message);
    }

    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool released_{false};
    std::vector<std::string> received_;
};

AlpacaCredentials fake_credentials() {
    auto loaded = AlpacaCredentials::from_environment([](std::string_view name) {
        return std::optional<std::string>(std::string(
            name == "APCA_API_KEY_ID" ? kFakeKeyId : kFakeSecret
        ));
    });
    REQUIRE(std::holds_alternative<AlpacaCredentials>(loaded));
    return std::get<AlpacaCredentials>(std::move(loaded));
}

WebSocketTransportTimeouts short_poll(std::chrono::milliseconds connect = 2s) {
    return WebSocketTransportTimeouts{
        .connect = connect,
        // Long enough that a slow machine does not stall mid-frame.
        .poll = 250ms,
        .write = 2s,
        .ping_interval = 10s,
        .maximum_missed_pongs = 2,
    };
}

// The next text frame, skipping idle polls; a slow machine may need several.
// The bound (about ten seconds of polls) only turns a hang into a failure.
std::optional<std::string> next_text(chronos::alpaca_adapter::AlpacaConnection& connection) {
    for (int poll = 0; poll < 40; ++poll) {
        auto read = connection.read();
        if (auto* text = std::get_if<TransportText>(&read)) {
            return std::move(text->payload);
        }
        if (!std::holds_alternative<TransportTimeout>(read)) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void test_transport_reads_text_timeouts_and_close() {
    FakeAlpacaServer server;
    auto transport = make_loopback_websocket_transport(server.url(), short_poll());
    auto connected = transport->connect();
    REQUIRE(std::holds_alternative<std::unique_ptr<chronos::alpaca_adapter::AlpacaConnection>>(connected));
    auto connection =
        std::get<std::unique_ptr<chronos::alpaca_adapter::AlpacaConnection>>(std::move(connected));

    REQUIRE(next_text(*connection) == std::optional<std::string>(R"([{"T":"success","msg":"connected"}])"));
    // Nothing else arrives until the client speaks.
    REQUIRE(std::holds_alternative<TransportTimeout>(connection->read()));
    REQUIRE(connection->send(R"({"action":"auth"})"));
    REQUIRE(next_text(*connection) == std::optional<std::string>(R"([{"T":"success","msg":"authenticated"}])"));
    REQUIRE(connection->send(R"({"action":"subscribe"})"));
    const auto acknowledgement = next_text(*connection);
    REQUIRE(acknowledgement.has_value());
    REQUIRE(contains(*acknowledgement, R"("T":"subscription")"));
    const auto quote = next_text(*connection);
    REQUIRE(quote.has_value());
    REQUIRE(contains(*quote, R"("bp":227.34)"));

    server.release();
    bool closed = false;
    for (int read = 0; read < 40 && !closed; ++read) {
        closed = std::holds_alternative<TransportClosed>(connection->read());
    }
    REQUIRE(closed);
    REQUIRE((server.received() ==
        std::vector<std::string>{R"({"action":"auth"})", R"({"action":"subscribe"})"}));
}

void test_transport_reports_a_refused_connection() {
    int unused_port = 0;
    {
        httplib::Server probe;
        unused_port = probe.bind_to_any_port("127.0.0.1");
    }
    auto transport = make_loopback_websocket_transport(
        "ws://127.0.0.1:" + std::to_string(unused_port) + "/v2/iex",
        short_poll(200ms)
    );
    auto refused = transport->connect();
    REQUIRE(std::holds_alternative<TransportConnectFailure>(refused));
    REQUIRE(!std::get<TransportConnectFailure>(refused).reason.empty());
}

void test_live_source_over_a_real_websocket() {
    FakeAlpacaServer server;
    ManualClock clock(ManualClock::Mode::Hold);
    const MonotonicUtcClock receive_clock(clock, Timestamp::from_nanoseconds(kReceiptStart));
    MarketState market{MarketDataStreamId("alpaca-iex-socket-test")};
    MarketDataNotifier notifier(clock, MarketDataNotifier::Options{});
    MarketStateFeed feed(market, notifier);
    auto subscription = AlpacaSubscription::create({InstrumentId("AAPL")});
    REQUIRE(std::holds_alternative<AlpacaSubscription>(subscription));

    AlpacaLiveSource source(
        make_loopback_websocket_transport(server.url(), short_poll()),
        fake_credentials(),
        std::get<AlpacaSubscription>(std::move(subscription)),
        feed,
        clock,
        receive_clock,
        [](MonotonicClock::duration bound) { return bound; }
    );
    if (!eventually([&] { return source.status().events_applied == 1U; })) {
        const auto status = source.status();
        throw chronos::test::TestFailure(
            "the quote was never applied; source " +
            std::string(chronos::market_data::state_name(status.state)) + ": " +
            status.diagnostic.value_or("no diagnostic") + "; server received " +
            std::to_string(server.received().size()) + " frames"
        );
    }
    REQUIRE(source.status().state == MarketDataSourceState::Live);
    const auto quote = market.execution_quote(InstrumentId("AAPL"));
    REQUIRE(std::holds_alternative<ExecutionQuote>(quote));
    REQUIRE(std::get<ExecutionQuote>(quote).top_of_book.bid->price == Price::parse("227.34"));

    const auto received = server.received();
    REQUIRE(received.size() == 2U);
    REQUIRE(contains(received[0], R"("action":"auth")"));
    REQUIRE(received[1] == R"({"action":"subscribe","trades":["AAPL"],"quotes":["AAPL"]})");

    // The server closes: the source reconnects and the quote is unusable.
    server.release();
    REQUIRE(eventually([&] {
        return source.status().state == MarketDataSourceState::Reconnecting;
    }));
    REQUIRE(!std::holds_alternative<ExecutionQuote>(market.execution_quote(InstrumentId("AAPL"))));
    source.stop();
    REQUIRE(source.status().state == MarketDataSourceState::Stopped);
    REQUIRE(!contains(source.status().diagnostic.value_or(""), kFakeSecret));
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"transport reads text, timeouts, and close", test_transport_reads_text_timeouts_and_close},
        {"transport reports a refused connection", test_transport_reports_a_refused_connection},
        {"live source over a real WebSocket", test_live_source_over_a_real_websocket},
    });
}
