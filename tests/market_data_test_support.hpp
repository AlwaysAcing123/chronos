#pragma once

#include "chronos/market_data/monotonic_clock.hpp"
#include "chronos/market_data_validation.hpp"
#include "chronos/market_event_reader.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::test {

inline constexpr std::string_view kSourceTestStream = "source-test-stream";
inline constexpr std::int64_t kRecordingStartNanoseconds =
    1'790'343'000'000'000'000;

// A clock that moves only when told. Waits are recorded so tests can assert
// the exact deadlines a component asked for.
//
// In Advance mode a timed wait jumps the clock to its deadline and returns,
// as if the time passed instantly. In Hold mode it blocks until woken or
// until the test advances the clock past the deadline, so tests can act
// while a wait is pending.
class ManualClock final : public market_data::MonotonicClock {
public:
    enum class Mode {
        Advance,
        Hold,
    };

    explicit ManualClock(
        Mode mode = Mode::Advance,
        time_point start = time_point(std::chrono::hours(1'000))
    )
        : mode_(mode),
          now_(start) {}

    [[nodiscard]] time_point now() const override {
        const std::lock_guard lock(mutex_);
        return now_;
    }

    bool wait_until(
        std::unique_lock<std::mutex>& lock,
        std::condition_variable& condition,
        time_point deadline,
        const std::function<bool()>& wake
    ) const override {
        Mode mode;
        {
            const std::lock_guard guard(mutex_);
            deadlines_.push_back(deadline);
            waiter_ = Waiter{.condition = &condition, .mutex = lock.mutex()};
            mode = mode_;
        }
        waits_changed_.notify_all();
        if (mode == Mode::Advance) {
            if (!wake()) {
                const std::lock_guard guard(mutex_);
                now_ = std::max(now_, deadline);
            }
        } else {
            condition.wait(lock, [&] { return wake() || now() >= deadline; });
        }
        {
            const std::lock_guard guard(mutex_);
            waiter_.reset();
        }
        return wake();
    }

    // Moves the clock and wakes a pending Hold-mode wait.
    void advance_to(time_point time) {
        std::optional<Waiter> waiter;
        {
            const std::lock_guard guard(mutex_);
            now_ = time;
            waiter = waiter_;
        }
        if (waiter) {
            const std::lock_guard guard(*waiter->mutex);
            waiter->condition->notify_all();
        }
    }

    [[nodiscard]] std::vector<time_point> deadlines() const {
        const std::lock_guard guard(mutex_);
        return deadlines_;
    }

    // Blocks until `count` timed waits have begun. The real-time bound only
    // turns a hang into a failure; passing tests never approach it.
    [[nodiscard]] bool await_waits(std::size_t count) const {
        std::unique_lock guard(mutex_);
        return waits_changed_.wait_for(guard, std::chrono::seconds(10), [&] {
            return deadlines_.size() >= count;
        });
    }

private:
    struct Waiter {
        std::condition_variable* condition;
        std::mutex* mutex;
    };

    mutable std::mutex mutex_;
    mutable std::condition_variable waits_changed_;
    Mode mode_;
    mutable time_point now_;
    mutable std::vector<time_point> deadlines_;
    mutable std::optional<Waiter> waiter_;
};

// A reader that returns a scripted sequence and then the end of the source.
class ScriptedReader final : public MarketEventReader {
public:
    explicit ScriptedReader(std::deque<MarketEventRead> reads)
        : reads_(std::move(reads)) {}

    [[nodiscard]] MarketEventRead read_next() override {
        if (reads_.empty()) {
            return EndOfMarketEvents{.source_sha256 = std::nullopt};
        }
        MarketEventRead next = std::move(reads_.front());
        reads_.pop_front();
        return next;
    }

private:
    std::deque<MarketEventRead> reads_;
};

inline ValidatedMarketEvent validated(MarketEvent event) {
    auto result = validate_market_event(std::move(event));
    REQUIRE(std::holds_alternative<ValidatedMarketEvent>(result));
    return std::get<ValidatedMarketEvent>(std::move(result));
}

// A valid two-sided quote received `offset` after the recording start.
inline ValidatedMarketEvent recorded_quote(
    std::uint64_t ingest_sequence,
    std::chrono::nanoseconds offset,
    std::string instrument = "AAPL",
    std::string_view stream = kSourceTestStream
) {
    const std::int64_t received = kRecordingStartNanoseconds + offset.count();
    return validated(TopOfBookEvent{
        .header = MarketEventHeader{
            .instrument = InstrumentId(std::move(instrument)),
            .stream = MarketDataStreamId(std::string(stream)),
            .venue = std::nullopt,
            .event_timestamp = Timestamp::from_nanoseconds(received - 100),
            .receive_timestamp = Timestamp::from_nanoseconds(received),
            .ingest_sequence = ingest_sequence,
            .venue_sequence = std::nullopt,
            .quality = MarketDataQuality{}.with(MarketDataQualityFlag::Replay),
        },
        .bid = BookLevel{.price = Price::parse("99"), .quantity = Quantity::parse("5")},
        .ask = BookLevel{.price = Price::parse("101"), .quantity = Quantity::parse("5")},
    });
}

// Waits for another thread to make `condition` true, by yielding rather
// than sleeping. The real-time bound only turns a hang into a failure.
template <typename Condition>
[[nodiscard]] bool eventually(Condition condition) {
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > give_up) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

}  // namespace chronos::test
