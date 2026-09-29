// The one place backend vocabulary becomes interface language. Closed enums
// use exhaustive records, so a new server value fails type checking here
// rather than rendering blank somewhere else.
import type { FailureCategory } from '../api/errors';
import type {
  AccountStatus,
  CancellationReason,
  MarketDataMode,
  MarketDataSourceState,
  OrderStatus,
  OrderType,
  QualityFlag,
  Side,
  SourceKind,
} from '../api/schemas';

// Every colour-coded state is also named in text.
export type Tone = 'positive' | 'negative' | 'caution' | 'neutral' | 'info';

export interface Labelled {
  readonly label: string;
  readonly tone: Tone;
}

export const ORDER_STATUS: Readonly<Record<OrderStatus, Labelled>> = {
  filled: { label: 'Filled', tone: 'positive' },
  partially_filled_and_cancelled: { label: 'Partially filled', tone: 'caution' },
  cancelled: { label: 'Cancelled', tone: 'neutral' },
  rejected: { label: 'Rejected', tone: 'negative' },
  faulted: { label: 'Faulted', tone: 'negative' },
};

export const CANCELLATION_REASON: Readonly<Record<CancellationReason, string>> = {
  limit_not_marketable: 'Limit price was not marketable against the displayed quote',
  displayed_liquidity_exhausted: 'Displayed liquidity was exhausted',
};

export const SIDE: Readonly<Record<Side, string>> = { buy: 'Buy', sell: 'Sell' };

export const ORDER_TYPE: Readonly<Record<OrderType, string>> = { market: 'Market', limit: 'Limit' };

export const SOURCE_KIND: Readonly<Record<SourceKind, string>> = { manual: 'Manual', model: 'Model' };

export const ACCOUNT_STATUS: Readonly<Record<AccountStatus, Labelled>> = {
  active: { label: 'Active', tone: 'positive' },
  halted: { label: 'Halted', tone: 'negative' },
};

export const QUALITY_FLAG: Readonly<Record<QualityFlag, Labelled>> = {
  snapshot: { label: 'Snapshot', tone: 'neutral' },
  replay: { label: 'Replay', tone: 'info' },
  questionable_receive_timestamp: { label: 'Questionable receive time', tone: 'caution' },
  potentially_invalid_book: { label: 'Potentially invalid book', tone: 'caution' },
  source_specific: { label: 'Source-specific', tone: 'neutral' },
};

export const MARKET_DATA_MODE: Readonly<Record<MarketDataMode, string>> = {
  none: 'No market data',
  historical_replay: 'Historical replay',
  live: 'Live data',
};

export const SOURCE_STATE: Readonly<Record<MarketDataSourceState, Labelled>> = {
  none: { label: 'No source', tone: 'neutral' },
  ready: { label: 'Ready to start', tone: 'neutral' },
  running: { label: 'Playing', tone: 'info' },
  paused: { label: 'Paused', tone: 'caution' },
  completed: { label: 'Completed', tone: 'neutral' },
  connecting: { label: 'Connecting', tone: 'caution' },
  authenticating: { label: 'Authenticating', tone: 'caution' },
  subscribing: { label: 'Subscribing', tone: 'caution' },
  live: { label: 'Live', tone: 'info' },
  reconnecting: { label: 'Reconnecting', tone: 'caution' },
  failed: { label: 'Failed', tone: 'negative' },
  stopped: { label: 'Stopped', tone: 'neutral' },
};

// Codes of recorded rejections and faults (order.rejection.code).
const RECORDED_OUTCOME: Readonly<Record<string, string>> = {
  risk_rejected: 'Rejected by pre-trade risk',
  market_data_stale: 'Rejected: the quote was too old',
  execution_rejected: 'Rejected by the execution simulator',
  processing_fault: 'Processing fault: the account is halted',
};

export function recordedOutcomeLabel(code: string): string {
  return RECORDED_OUTCOME[code] ?? code;
}

// Reason codes are an open vocabulary shared by risk, execution, market
// data, valuation, and request parsing. Unknown ones are shown verbatim.
const REASON: Readonly<Record<string, string>> = {
  // Pre-trade risk
  // Recorded only before accounts could hold several instruments.
  instrument_mismatch: "The order instrument differed from the account's single instrument",
  nonpositive_quantity: 'Quantity must be positive',
  nonpositive_limit_price: 'Limit price must be positive',
  missing_limit_price: 'A limit order needs a limit price',
  unexpected_limit_price: 'A market order cannot have a limit price',
  submission_timestamp_moved_backward: "Market time is earlier than the account's latest fill",
  market_observation_from_future: 'The quote is later than market time',
  missing_opposite_book_side: 'No displayed liquidity on the side this order needs',
  order_quantity_limit_exceeded: 'Exceeds the maximum order quantity',
  order_notional_limit_exceeded: 'Exceeds the maximum order notional',
  notional_rounded_to_zero: 'The order notional rounds to zero',
  position_limit_exceeded: 'Would exceed the maximum position',
  insufficient_cash: 'Not enough cash for the order and fee',
  insufficient_cash_for_fee: 'Not enough cash for the fee',
  insufficient_position: 'Not enough position to sell',
  arithmetic_overflow: 'A value exceeds the supported fixed-point range',
  // Execution
  duplicate_client_order_id: 'The client order ID was already processed',
  market_stream_mismatch: "The market is a different stream than the account's",
  stale_market_approval: 'The order used an older market observation',
  market_snapshot_mismatch: 'One sequence number identified two market states',
  // Processing faults
  processor_faulted: 'The account is halted after an earlier fault',
  accounting_invariant_violation: 'Accounting rejected a risk-approved fill',
  // Market data and valuation
  no_top_of_book: 'No quote has been received for this instrument',
  stream_interrupted: 'The market-data stream was interrupted; the server must be restarted',
  source_unavailable: 'The market-data source is unavailable, so no quote is current',
  awaiting_fresh_quote: 'Waiting for a new quote for this instrument since the market-data source resumed',
  missing_best_bid: 'The book has no bid to mark the position',
  suspect_market_state: 'The quote is flagged as a potentially invalid book or questionable receive time',
  stale_market_state: "The quote is older than the account's maximum quote age",
  incompatible_market_timeline: "The served market does not continue this account's timeline",
  invalid_market_state: 'The quote fails the market-data contract',
  market_instrument_mismatch: 'The quote is for another instrument',
  nonpositive_starting_cash: 'The account has no positive starting cash',
  accounting_identity_failed: 'The valuation did not reconcile',
  position_mark_unavailable: 'A held position has no usable quote',
  // Request parsing
  invalid_identifier: 'Use 1–64 letters, digits, ".", "_" or "-", starting with a letter or digit',
  too_many_fractional_digits: 'Too many decimal places for this field',
  decimal_out_of_range: 'The value is outside the supported range',
  invalid_decimal: 'Not a valid decimal number',
};

export function reasonLabel(reason: string): string {
  return REASON[reason] ?? reason;
}

export interface FailureDescription {
  readonly title: string;
  readonly detail: string;
}

export const FAILURE: Readonly<Record<FailureCategory, FailureDescription>> = {
  malformed_request: {
    title: 'The server rejected the request',
    detail: 'Nothing was recorded. Correct the highlighted value and try again.',
  },
  account_not_found: {
    title: 'Paper account not found',
    detail: 'The server has no paper account with this ID. It may belong to a different database.',
  },
  order_not_found: {
    title: 'Order not found',
    detail: 'The account has no order with this client order ID.',
  },
  account_already_exists: {
    title: 'Account ID already in use',
    detail: 'A paper account with this ID already exists. Open it instead, or choose another ID.',
  },
  duplicate_client_order_id: {
    title: 'Client order ID already used',
    detail: 'The account already recorded an order with this client order ID.',
  },
  account_version_conflict: {
    title: 'The account changed during submission',
    detail: 'Another order was processed first. Nothing was recorded, so the same order can be sent again.',
  },
  unsupported_instrument: {
    title: 'Instrument not supported',
    detail: 'The server does not trade this instrument.',
  },
  invalid_starting_cash: {
    title: 'Starting cash must be positive',
    detail: 'Enter a positive amount of simulated cash.',
  },
  market_data_unavailable: {
    title: 'Market data unavailable',
    detail: 'The server has no usable quote for this right now. Nothing was recorded.',
  },
  valuation_failed: {
    title: 'The portfolio could not be valued exactly',
    detail: 'The server refused to return values it could not reconcile.',
  },
  persistence_unavailable: {
    title: 'The paper-trading database is unavailable',
    detail: 'The server could not reach PostgreSQL. Reads can be retried; a write may or may not have completed.',
  },
  internal_error: {
    title: 'The server hit an unexpected error',
    detail: 'Details are in the server log.',
  },
  invalid_replay_transition: {
    title: 'The replay cannot do that now',
    detail: 'The replay changed state before the request arrived; its current state is shown.',
  },
  replay_control_unavailable: {
    title: 'This server is not replaying a recording',
    detail: 'Replay controls apply only to a historical replay source.',
  },
  unexpected_response: {
    title: 'Unexpected response',
    detail: 'The response did not match the Chronos API contract, so it is not shown.',
  },
  network: {
    title: 'The Chronos API could not be reached',
    detail: 'Check that the C++ server is running and reachable through the development proxy.',
  },
};
