#pragma once

#include "chronos/alpaca/alpaca_transport.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::test {

// The test's side of one fake WebSocket connection: it scripts what the
// source reads and inspects what the source sent. Reads block until an
// item is scripted or the transport is interrupted, so no test sleeps.
class FakeLink final {
public:
    void push(std::string frame) {
        enqueue(alpaca_adapter::TransportText{.payload = std::move(frame)});
    }
    // One idle poll, as the real transport reports once a second.
    void push_timeout() { enqueue(alpaca_adapter::TransportTimeout{}); }
    void push_binary() { enqueue(alpaca_adapter::TransportBinary{}); }
    void close(std::string reason = "closed by peer") {
        enqueue(alpaca_adapter::TransportClosed{.reason = std::move(reason)});
    }

    [[nodiscard]] std::vector<std::string> sent() const {
        const std::lock_guard lock(mutex_);
        return sent_;
    }

    // Blocks until the source has sent `count` frames. The real-time bound
    // only turns a hang into a failure.
    [[nodiscard]] bool await_sent(std::size_t count) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(10), [&] {
            return sent_.size() >= count;
        });
    }

    // Blocks until the source has read everything scripted so far.
    [[nodiscard]] bool await_drained() const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(10), [&] {
            return inbound_.empty() && reading_;
        });
    }

    [[nodiscard]] bool released() const {
        const std::lock_guard lock(mutex_);
        return released_;
    }

    alpaca_adapter::TransportRead read() {
        std::unique_lock lock(mutex_);
        reading_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return !inbound_.empty() || interrupted_; });
        reading_ = false;
        if (inbound_.empty()) {
            return alpaca_adapter::TransportTimeout{};
        }
        alpaca_adapter::TransportRead next = std::move(inbound_.front());
        inbound_.pop_front();
        changed_.notify_all();
        return next;
    }

    bool send(const std::string& text) {
        const std::lock_guard lock(mutex_);
        sent_.push_back(text);
        changed_.notify_all();
        return true;
    }

    void interrupt() {
        const std::lock_guard lock(mutex_);
        interrupted_ = true;
        changed_.notify_all();
    }

    void release() {
        const std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    void enqueue(alpaca_adapter::TransportRead read) {
        const std::lock_guard lock(mutex_);
        inbound_.push_back(std::move(read));
        changed_.notify_all();
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::deque<alpaca_adapter::TransportRead> inbound_;
    std::vector<std::string> sent_;
    bool reading_{false};
    bool interrupted_{false};
    bool released_{false};
};

class FakeConnection final : public alpaca_adapter::AlpacaConnection {
public:
    explicit FakeConnection(std::shared_ptr<FakeLink> link) : link_(std::move(link)) {}
    ~FakeConnection() override { link_->release(); }

    FakeConnection(const FakeConnection&) = delete;
    FakeConnection& operator=(const FakeConnection&) = delete;
    FakeConnection(FakeConnection&&) = delete;
    FakeConnection& operator=(FakeConnection&&) = delete;

    [[nodiscard]] alpaca_adapter::TransportRead read() override { return link_->read(); }
    [[nodiscard]] bool send(const std::string& text) override { return link_->send(text); }

private:
    std::shared_ptr<FakeLink> link_;
};

// Each connect() takes the next scripted outcome, waiting for one to be
// scripted unless the transport is interrupted.
class FakeTransport final : public alpaca_adapter::AlpacaTransport {
public:
    std::shared_ptr<FakeLink> accept() {
        auto link = std::make_shared<FakeLink>();
        const std::lock_guard lock(mutex_);
        script_.emplace_back(link);
        changed_.notify_all();
        return link;
    }

    void refuse(std::string reason) {
        const std::lock_guard lock(mutex_);
        script_.emplace_back(alpaca_adapter::TransportConnectFailure{.reason = std::move(reason)});
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t connects() const {
        const std::lock_guard lock(mutex_);
        return connects_;
    }

    // Blocks until connect() has been entered `count` times.
    [[nodiscard]] bool await_connect_attempts(std::size_t count) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(10), [&] {
            return attempts_ >= count;
        });
    }

    [[nodiscard]] alpaca_adapter::TransportConnectResult connect() override {
        std::unique_lock lock(mutex_);
        ++attempts_;
        changed_.notify_all();
        changed_.wait(lock, [&] { return !script_.empty() || interrupted_; });
        if (script_.empty()) {
            return alpaca_adapter::TransportConnectFailure{.reason = "interrupted"};
        }
        Outcome next = std::move(script_.front());
        script_.pop_front();
        ++connects_;
        changed_.notify_all();
        if (auto* failure = std::get_if<alpaca_adapter::TransportConnectFailure>(&next)) {
            return std::move(*failure);
        }
        auto link = std::get<std::shared_ptr<FakeLink>>(std::move(next));
        if (interrupted_) {
            link->interrupt();
        }
        links_.push_back(link);
        return std::unique_ptr<alpaca_adapter::AlpacaConnection>(
            std::make_unique<FakeConnection>(std::move(link))
        );
    }

    void interrupt() noexcept override {
        std::vector<std::shared_ptr<FakeLink>> links;
        {
            const std::lock_guard lock(mutex_);
            interrupted_ = true;
            links = links_;
            changed_.notify_all();
        }
        for (const auto& link : links) {
            link->interrupt();
        }
    }

private:
    using Outcome = std::variant<
        std::shared_ptr<FakeLink>,
        alpaca_adapter::TransportConnectFailure
    >;

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::deque<Outcome> script_;
    std::vector<std::shared_ptr<FakeLink>> links_;
    std::size_t attempts_{0};
    std::size_t connects_{0};
    bool interrupted_{false};
};

}  // namespace chronos::test
