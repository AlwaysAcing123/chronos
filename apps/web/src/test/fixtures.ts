// JSON exactly as the C++ API renders it, built from the documented example
// of the recording's final AAPL quote (docs/paper-trading-api.md). Shared by
// component tests and the Playwright suite, so it must stay framework-free.

export type Json = Record<string, unknown>;

export const ACCOUNT_ID = 'demo-1';
export const STREAM_ID = 'databento-equs-mini-mbp1-20260925-1330-1340';
export const MARKET_TIME = '2026-09-25T13:39:59.969574414Z';

export function healthJson(overrides: Json = {}): Json {
  return {
    status: 'ok',
    trading_mode: 'paper',
    persistence: 'available',
    tradeable_instruments: ['AAPL', 'SPY'],
    market_data: {
      stream_id: STREAM_ID,
      state: 'accepting',
      last_ingest_sequence: '121302',
      market_time: MARKET_TIME,
      interruption: null,
    },
    ...overrides,
  };
}

export function marketStreamJson(overrides: Json = {}): Json {
  return {
    stream_id: STREAM_ID,
    state: 'accepting',
    last_ingest_sequence: '121302',
    market_time: MARKET_TIME,
    interruption: null,
    ...overrides,
  };
}

export function replaySourceJson(overrides: Json = {}): Json {
  return {
    mode: 'historical_replay',
    provider: 'Databento',
    feed: 'EQUS.MINI MBP-1',
    label: 'Databento EQUS.MINI MBP-1 replay',
    coverage_notice: null,
    coverage_label: null,
    state: 'running',
    replay_speed: '10x',
    events_applied: '121303',
    diagnostic: null,
    recording_sha256: null,
    instruments: ['AAPL', 'SPY'],
    ...overrides,
  };
}

export const IEX_COVERAGE_NOTICE =
  'IEX only: quotes and trades from the Investors Exchange alone. This is not consolidated SIP data, not the NBBO, and not every US exchange.';

export function liveSourceJson(overrides: Json = {}): Json {
  return {
    mode: 'live',
    provider: 'Alpaca',
    feed: 'IEX',
    label: 'Alpaca IEX',
    coverage_notice: IEX_COVERAGE_NOTICE,
    coverage_label: 'IEX only, not SIP or NBBO',
    state: 'live',
    replay_speed: null,
    events_applied: '42',
    diagnostic: null,
    recording_sha256: null,
    instruments: ['AAPL', 'SPY'],
    ...overrides,
  };
}

export function marketDataJson({
  source = replaySourceJson(),
  stream = marketStreamJson(),
  revision = '7',
  available = true,
  reasons,
}: {
  source?: Json;
  stream?: Json;
  revision?: string;
  available?: boolean;
  // Per-instrument unavailability; null means available.
  reasons?: Readonly<Record<string, string | null>>;
} = {}): Json {
  return {
    trading_mode: 'paper',
    revision,
    source,
    stream,
    instruments: ['AAPL', 'SPY'].map((instrument) => {
      const reason = reasons ? (reasons[instrument] ?? null) : available ? null : 'no_top_of_book';
      return {
        instrument,
        execution_quote_available: reason === null,
        unavailable_reason: reason,
      };
    }),
  };
}

export function riskLimitsJson(): Json {
  return {
    maximum_order_quantity: '1000',
    maximum_order_notional: '1000000',
    maximum_position_quantity: '10000',
    maximum_quote_age_ns: '5000000000',
    fee_per_fill: '0.35',
  };
}

export function accountJson(overrides: Json = {}): Json {
  return {
    trading_mode: 'paper',
    account_id: ACCOUNT_ID,
    instrument: 'AAPL',
    version: '2',
    status: 'active',
    starting_cash: '100000',
    cash: '99155.03',
    position: '2.5',
    open_cost_basis: '842.5',
    average_entry_price: '337',
    gross_realized_pnl: '-1.77',
    fees_paid: '0.7',
    last_fill_at: MARKET_TIME,
    risk_limits: riskLimitsJson(),
    ...overrides,
  };
}

export function flatAccountJson(overrides: Json = {}): Json {
  return accountJson({
    version: '0',
    cash: '100000',
    position: '0',
    open_cost_basis: '0',
    average_entry_price: null,
    gross_realized_pnl: '0',
    fees_paid: '0',
    last_fill_at: null,
    ...overrides,
  });
}

export function marketJson(overrides: Json = {}): Json {
  return {
    stream_id: STREAM_ID,
    ingest_sequence: '121297',
    venue: null,
    event_timestamp: '2026-09-25T13:39:59.960824207Z',
    receive_timestamp: '2026-09-25T13:39:59.960922473Z',
    quality_flags: ['replay'],
    bid: { price: '335.82', quantity: '80' },
    ask: { price: '337', quantity: '4' },
    ...overrides,
  };
}

export function portfolioJson(overrides: Json = {}): Json {
  return {
    trading_mode: 'paper',
    account_id: ACCOUNT_ID,
    instrument: 'AAPL',
    account_version: '2',
    status: 'active',
    valued_at: MARKET_TIME,
    mark_policy: 'best_bid',
    mark_price: '335.82',
    market: marketJson(),
    starting_cash: '100000',
    cash: '99155.03',
    position: '2.5',
    open_cost_basis: '842.5',
    average_entry_price: '337',
    position_market_value: '839.55',
    unrealized_pnl: '-2.95',
    gross_realized_pnl: '-1.77',
    fees_paid: '0.7',
    total_pnl: '-5.42',
    equity: '99994.58',
    return_percent: '-0.00542',
    ...overrides,
  };
}

// The documented 10-share market buy that filled 4 against the displayed ask.
export function partialOrderJson(overrides: Json = {}): Json {
  return {
    trading_mode: 'paper',
    account_id: ACCOUNT_ID,
    client_order_id: 'buy-1',
    source: { kind: 'manual', id: 'web-ticket' },
    instrument: 'AAPL',
    side: 'buy',
    type: 'market',
    quantity: '10',
    limit_price: null,
    time_in_force: 'ioc',
    submitted_at: MARKET_TIME,
    status: 'partially_filled_and_cancelled',
    filled_quantity: '4',
    unfilled_quantity: '6',
    cancellation_reason: 'displayed_liquidity_exhausted',
    fill: {
      fill_id: 'buy-1:fill:0',
      side: 'buy',
      price: '337',
      quantity: '4',
      notional: '1348',
      fee: '0.35',
      filled_at: MARKET_TIME,
    },
    rejection: null,
    market: marketJson(),
    ...overrides,
  };
}

export function filledOrderJson(overrides: Json = {}): Json {
  const clientOrderId = typeof overrides.client_order_id === 'string' ? overrides.client_order_id : 'sell-1';
  return partialOrderJson({
    client_order_id: clientOrderId,
    side: 'sell',
    type: 'limit',
    quantity: '1.5',
    limit_price: '300',
    status: 'filled',
    filled_quantity: '1.5',
    unfilled_quantity: '0',
    cancellation_reason: null,
    fill: {
      fill_id: `${clientOrderId}:fill:0`,
      side: 'sell',
      price: '335.82',
      quantity: '1.5',
      notional: '503.73',
      fee: '0.35',
      filled_at: MARKET_TIME,
    },
    ...overrides,
  });
}

export function cancelledOrderJson(overrides: Json = {}): Json {
  return partialOrderJson({
    client_order_id: 'buy-low',
    type: 'limit',
    quantity: '1',
    limit_price: '300',
    status: 'cancelled',
    filled_quantity: '0',
    unfilled_quantity: '1',
    cancellation_reason: 'limit_not_marketable',
    fill: null,
    ...overrides,
  });
}

export function riskRejectionJson(): Json {
  return {
    code: 'risk_rejected',
    message: 'paper account lacks cash for the order and fee',
    field: 'quantity',
    reason: 'insufficient_cash',
  };
}

export function rejectedOrderJson(overrides: Json = {}): Json {
  return partialOrderJson({
    client_order_id: 'buy-big',
    quantity: '900',
    status: 'rejected',
    filled_quantity: '0',
    unfilled_quantity: '900',
    cancellation_reason: null,
    fill: null,
    rejection: riskRejectionJson(),
    ...overrides,
  });
}

export function faultedOrderJson(overrides: Json = {}): Json {
  return partialOrderJson({
    client_order_id: 'buy-fault',
    status: 'faulted',
    filled_quantity: '0',
    unfilled_quantity: '10',
    cancellation_reason: null,
    fill: null,
    rejection: {
      code: 'processing_fault',
      message: 'accounting rejected the simulated fill',
      field: null,
      reason: 'accounting_invariant_violation',
    },
    ...overrides,
  });
}

export function fillJson(overrides: Json = {}): Json {
  return {
    trading_mode: 'paper',
    account_id: ACCOUNT_ID,
    client_order_id: 'buy-1',
    source: { kind: 'manual', id: 'web-ticket' },
    instrument: 'AAPL',
    fill_id: 'buy-1:fill:0',
    side: 'buy',
    price: '337',
    quantity: '4',
    notional: '1348',
    fee: '0.35',
    filled_at: MARKET_TIME,
    ...overrides,
  };
}

export function errorJson(
  code: string,
  message: string,
  { field = null, reason = null }: { field?: string | null; reason?: string | null } = {},
): Json {
  return { error: { code, message, field, reason } };
}

export const PERSISTENCE_UNAVAILABLE_MESSAGE =
  'paper-trading persistence is unavailable; if this request writes data, it may or may not ' +
  'have completed, so retry the same request with the same identifiers';
