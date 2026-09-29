// How often each resource is re-read. Polling is the interim transport: a
// later live-update subscription can write the same query keys and set these
// intervals to false without changing any component.
export interface RefreshPolicy {
  readonly healthMs: number | false;
  readonly accountMs: number | false;
}

export const POLLING_REFRESH_POLICY: RefreshPolicy = {
  healthMs: 5_000,
  // The selected account, its portfolio, and the newest history pages.
  accountMs: 2_000,
};
