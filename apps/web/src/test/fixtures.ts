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
    synthetic: false,
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

export const SYNTHETIC_COVERAGE_NOTICE =
  'These events are synthetic: invented events of scenario chronos.demo_scenario version 1, not market data. Their prices, sizes, and times were generated, not observed on any exchange (chronos.synthetic.event_table version 1), so nothing here describes a real market.';

// A demo workspace's replay of a synthetic dataset (ADR 0025).
export function syntheticSourceJson(overrides: Json = {}): Json {
  return replaySourceJson({
    synthetic: true,
    provider: 'synthetic',
    feed: 'chronos.demo_scenario event_table',
    label: 'Synthetic replay: invented prices, not market data',
    coverage_notice: SYNTHETIC_COVERAGE_NOTICE,
    coverage_label: 'Synthetic, not market data',
    state: 'ready',
    replay_speed: '1x',
    events_applied: '0',
    ...overrides,
  });
}

export function liveSourceJson(overrides: Json = {}): Json {
  return {
    mode: 'live',
    synthetic: false,
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
  quotes = {},
}: {
  source?: Json;
  stream?: Json;
  revision?: string;
  available?: boolean;
  // Per-instrument unavailability; null means available.
  reasons?: Readonly<Record<string, string | null>>;
  // Replaces an available instrument's quote.
  quotes?: Readonly<Record<string, Json>>;
} = {}): Json {
  const quoteOf: Readonly<Record<string, Json>> = { AAPL: marketJson(), SPY: spyMarketJson(), ...quotes };
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
        quote: reason === null ? quoteOf[instrument] : null,
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

// The documented AAPL position: 2.5 held at an average of 337.
export function openPositionJson(overrides: Json = {}): Json {
  const quantity = typeof overrides.quantity === 'string' ? overrides.quantity : '2.5';
  return {
    instrument: 'AAPL',
    quantity: '2.5',
    reserved_quantity: '0',
    available_quantity: quantity,
    open_cost_basis: '842.5',
    average_entry_price: '337',
    gross_realized_pnl: '-1.77',
    fees_paid: '0.7',
    last_fill_at: MARKET_TIME,
    ...overrides,
  };
}

export function closedPositionJson(overrides: Json = {}): Json {
  return {
    instrument: 'SPY',
    gross_realized_pnl: '2.5',
    fees_paid: '0.7',
    last_fill_at: MARKET_TIME,
    ...overrides,
  };
}

export function accountJson(overrides: Json = {}): Json {
  return {
    trading_mode: 'paper',
    account_id: ACCOUNT_ID,
    version: '2',
    status: 'active',
    market_stream_id: STREAM_ID,
    starting_cash: '100000',
    cash: '99155.03',
    reserved_cash: '0',
    available_cash: typeof overrides.cash === 'string' ? overrides.cash : '99155.03',
    working_order_count: '0',
    open_cost_basis: '842.5',
    gross_realized_pnl: '-1.77',
    fees_paid: '0.7',
    last_fill_at: MARKET_TIME,
    positions: [openPositionJson()],
    closed_positions: [],
    risk_limits: riskLimitsJson(),
    ...overrides,
  };
}

export function flatAccountJson(overrides: Json = {}): Json {
  return accountJson({
    version: '0',
    market_stream_id: null,
    cash: '100000',
    open_cost_basis: '0',
    gross_realized_pnl: '0',
    fees_paid: '0',
    last_fill_at: null,
    positions: [],
    closed_positions: [],
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

// The recording's final SPY quote, the latest event of the replay.
export function spyMarketJson(overrides: Json = {}): Json {
  return marketJson({
    ingest_sequence: '121302',
    event_timestamp: '2026-09-25T13:39:59.969499529Z',
    receive_timestamp: '2026-09-25T13:39:59.969574414Z',
    bid: { price: '769.7', quantity: '50' },
    ask: { price: '769.78', quantity: '340' },
    ...overrides,
  });
}

// The documented AAPL position marked at its 335.82 bid.
export function valuedPositionJson(overrides: Json = {}): Json {
  return {
    ...openPositionJson(typeof overrides.quantity === 'string' ? { quantity: overrides.quantity } : {}),
    mark_price: '335.82',
    market: marketJson(),
    market_value: '839.55',
    unrealized_pnl: '-2.95',
    ...overrides,
  };
}

export function portfolioJson(overrides: Json = {}): Json {
  return {
    trading_mode: 'paper',
    account_id: ACCOUNT_ID,
    account_version: '2',
    status: 'active',
    market_stream_id: STREAM_ID,
    valued_at: MARKET_TIME,
    mark_policy: 'best_bid',
    starting_cash: '100000',
    cash: '99155.03',
    reserved_cash: '0',
    available_cash: typeof overrides.cash === 'string' ? overrides.cash : '99155.03',
    open_cost_basis: '842.5',
    market_value: '839.55',
    unrealized_pnl: '-2.95',
    gross_realized_pnl: '-1.77',
    fees_paid: '0.7',
    total_pnl: '-5.42',
    equity: '99994.58',
    return_percent: '-0.00542',
    positions: [valuedPositionJson()],
    closed_positions: [],
    ...overrides,
  };
}

// A flat account's valuation: its cash, with nothing to mark.
export function flatPortfolioJson(overrides: Json = {}): Json {
  return portfolioJson({
    account_version: '0',
    market_stream_id: null,
    cash: '100000',
    open_cost_basis: '0',
    market_value: '0',
    unrealized_pnl: '0',
    gross_realized_pnl: '0',
    fees_paid: '0',
    total_pnl: '0',
    equity: '100000',
    return_percent: '0',
    positions: [],
    closed_positions: [],
    ...overrides,
  });
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
    remaining_quantity: '6',
    unfilled_quantity: '6',
    filled_notional: '1348',
    fill_count: '1',
    fee_charged: true,
    cumulative_fee: '0.35',
    reserved_cash: '0',
    reserved_quantity: '0',
    priority: null,
    updated_at: MARKET_TIME,
    cancellation_reason: 'displayed_liquidity_exhausted',
    cancel_request_id: null,
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
    remaining_quantity: '0',
    unfilled_quantity: '0',
    filled_notional: '503.73',
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
    remaining_quantity: '1',
    unfilled_quantity: '1',
    filled_notional: '0',
    fill_count: '0',
    fee_charged: false,
    cumulative_fee: '0',
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
    remaining_quantity: '900',
    unfilled_quantity: '900',
    filled_notional: '0',
    fill_count: '0',
    fee_charged: false,
    cumulative_fee: '0',
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
    remaining_quantity: '10',
    unfilled_quantity: '10',
    filled_notional: '0',
    fill_count: '0',
    fee_charged: false,
    cumulative_fee: '0',
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
    fill_index: '0',
    ...overrides,
  };
}

// A good-til-cancelled buy of 10 at 336 that rests below the 337 ask: it
// reserves 336 × 10 = 3360 plus the 0.35 fee until its first fill.
export function workingOrderJson(overrides: Json = {}): Json {
  return partialOrderJson({
    client_order_id: 'gtc-1',
    type: 'limit',
    quantity: '10',
    limit_price: '336',
    time_in_force: 'gtc',
    status: 'working',
    filled_quantity: '0',
    remaining_quantity: '10',
    unfilled_quantity: '10',
    filled_notional: '0',
    fill_count: '0',
    fee_charged: false,
    cumulative_fee: '0',
    reserved_cash: '3360.35',
    reserved_quantity: '0',
    priority: '41',
    cancellation_reason: null,
    fill: null,
    ...overrides,
  });
}

// The same order after a later ask of 336 × 4 filled 4 of it: the fee is
// paid, and the 6 still working reserve 336 × 6 = 2016.
export function partiallyFilledWorkingOrderJson(overrides: Json = {}): Json {
  return workingOrderJson({
    status: 'partially_filled',
    filled_quantity: '4',
    remaining_quantity: '6',
    unfilled_quantity: '6',
    filled_notional: '1344',
    fill_count: '1',
    fee_charged: true,
    cumulative_fee: '0.35',
    reserved_cash: '2016',
    updated_at: '2026-09-25T13:40:00.000000000Z',
    ...overrides,
  });
}

// Its remainder cancelled at the client's request: the fill and fee stand,
// and nothing stays reserved.
export function userCancelledOrderJson(overrides: Json = {}): Json {
  return partiallyFilledWorkingOrderJson({
    status: 'partially_filled_and_cancelled',
    reserved_cash: '0',
    cancellation_reason: 'user_requested',
    cancel_request_id: 'cancel-1',
    updated_at: '2026-09-25T13:40:01.000000000Z',
    ...overrides,
  });
}

export function orderEventJson(overrides: Json = {}): Json {
  return {
    sequence: '1',
    type: 'order_submitted',
    occurred_at: MARKET_TIME,
    fill: null,
    ...overrides,
  };
}

export function errorJson(
  code: string,
  message: string,
  {
    field = null,
    reason = null,
    instruments,
  }: { field?: string | null; reason?: string | null; instruments?: readonly Json[] } = {},
): Json {
  return { error: { code, message, field, reason, ...(instruments === undefined ? {} : { instruments }) } };
}

// A portfolio that cannot be valued because held positions have no mark.
export function unmarkableJson(failures: Readonly<Record<string, string>>): Json {
  const count = Object.keys(failures).length;
  return errorJson(
    'market_data_unavailable',
    count === 1 ? 'an open position cannot be marked' : `${String(count)} open positions cannot be marked`,
    {
      reason: 'position_mark_unavailable',
      instruments: Object.entries(failures).map(([instrument, reason]) => ({
        instrument,
        reason,
        message: `no usable ${instrument} quote`,
      })),
    },
  );
}

export const PERSISTENCE_UNAVAILABLE_MESSAGE =
  'paper-trading persistence is unavailable; if this request writes data, it may or may not ' +
  'have completed, so retry the same request with the same identifiers';
