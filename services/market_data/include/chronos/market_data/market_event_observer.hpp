#pragma once

#include "chronos/market_data_validation.hpp"

namespace chronos::market_data {

// Sees every event the market state accepts, in stream order, on the
// source's thread: after the event is applied and before subscribers are
// told or the next event is applied. Unlike MarketDataNotifier, nothing is
// coalesced or skipped.
//
// observe() may block. The source then waits, which is the only
// backpressure: no event is buffered or dropped. It must not call back into
// the feed. An exception fails the source, as a rejected event does.
class MarketEventObserver {
public:
    virtual ~MarketEventObserver() = default;

    virtual void observe(const ValidatedMarketEvent& event) = 0;

protected:
    MarketEventObserver() = default;
    MarketEventObserver(const MarketEventObserver&) = default;
    MarketEventObserver& operator=(const MarketEventObserver&) = default;
};

}  // namespace chronos::market_data
