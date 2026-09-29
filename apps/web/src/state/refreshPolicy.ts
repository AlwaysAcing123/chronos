// How often each resource is re-read. Market-dependent resources follow the
// server's change stream when it is open and fall back to polling when it
// is not; durable account resources are always polled, because other
// clients can record orders.
export interface RefreshPolicy {
  readonly healthMs: number | false;
  // The selected account and its newest history pages.
  readonly accountMs: number | false;
  // Market data and portfolio valuation without a change stream.
  readonly marketMs: number | false;
  // The same while the change stream is open: only a safety net.
  readonly streamedMarketMs: number | false;
}

export const POLLING_REFRESH_POLICY: RefreshPolicy = {
  healthMs: 5_000,
  accountMs: 2_000,
  marketMs: 2_000,
  streamedMarketMs: 15_000,
};
