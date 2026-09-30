// A small stateful stand-in for the C++ API, answered inside the browser.
// Every body is contract JSON from the shared fixtures, and every response
// carries the headers the real server sends.
import type { Page, Route } from '@playwright/test';

import type { Json } from '../src/test/fixtures';
import {
  errorJson,
  fillJson,
  flatAccountJson,
  flatPortfolioJson,
  healthJson,
  liveSourceJson,
  marketDataJson,
  MARKET_TIME,
  openPositionJson,
  orderEventJson,
  partialOrderJson,
  partiallyFilledWorkingOrderJson,
  portfolioJson,
  replaySourceJson,
  spyMarketJson,
  userCancelledOrderJson,
  valuedPositionJson,
  workingOrderJson,
} from '../src/test/fixtures';

// One good-til-cancelled order per account: a buy of 10 AAPL at 336, below
// the 337 ask, which a later ask of 336 × 4 partially fills.
interface MockWorkingOrder {
  clientOrderId: string;
  source: Json;
  stage: 'working' | 'partially_filled' | 'cancelled';
  cancelRequestId: string | null;
}

interface MockAccount {
  accountId: string;
  // Which canned fills the account has booked.
  holds: { aapl: boolean; spy: boolean };
  orders: Json[];
  fills: Json[];
  gtc: MockWorkingOrder | null;
}

export interface MockOptions {
  // Fail the first submission in transit even though it was recorded, as
  // when a response is lost after the commit.
  readonly loseFirstSubmissionResponse?: boolean;
  // Serve an Alpaca IEX live source instead of a historical replay.
  readonly liveSource?: boolean;
}

// Every AAPL submission is the documented 10-share market buy that filled 4
// at 337 with a 0.35 fee, marked at the 335.82 bid; every SPY submission
// fills 2 at the recording's final 769.78 ask with a 0.35 fee, marked at its
// 769.7 bid. The totals below follow the server's accounting rules for those
// fills, worked out by hand; the mock does no arithmetic.
const AAPL_POSITION = { quantity: '4', open_cost_basis: '1348', average_entry_price: '337', gross_realized_pnl: '0', fees_paid: '0.35' };
const AAPL_MARK = { mark_price: '335.82', market_value: '1343.28', unrealized_pnl: '-4.72' };
const SPY_POSITION = { instrument: 'SPY', quantity: '2', open_cost_basis: '1539.56', average_entry_price: '769.78', gross_realized_pnl: '0', fees_paid: '0.35' };
const SPY_MARK = { mark_price: '769.7', market: spyMarketJson(), market_value: '1539.4', unrealized_pnl: '-0.16' };

const TOTALS: Readonly<Record<string, Json>> = {
  aapl: { cash: '98651.65', open_cost_basis: '1348', fees_paid: '0.35', market_value: '1343.28', unrealized_pnl: '-4.72', total_pnl: '-5.07', equity: '99994.93', return_percent: '-0.00507' },
  spy: { cash: '98460.09', open_cost_basis: '1539.56', fees_paid: '0.35', market_value: '1539.4', unrealized_pnl: '-0.16', total_pnl: '-0.51', equity: '99999.49', return_percent: '-0.00051' },
  'aapl+spy': { cash: '97111.74', open_cost_basis: '2887.56', fees_paid: '0.7', market_value: '2882.68', unrealized_pnl: '-4.88', total_pnl: '-5.58', equity: '99994.42', return_percent: '-0.00558' },
};

const GTC_FILLED_AT = '2026-09-25T13:40:00.000000000Z';

// The working order's resource at each stage, from the shared fixtures.
function gtcOrder(accountId: string, gtc: MockWorkingOrder): Json {
  const common = { account_id: accountId, client_order_id: gtc.clientOrderId, source: gtc.source };
  switch (gtc.stage) {
    case 'working':
      return workingOrderJson(common);
    case 'partially_filled':
      return partiallyFilledWorkingOrderJson(common);
    case 'cancelled':
      return userCancelledOrderJson({ ...common, cancel_request_id: gtc.cancelRequestId });
  }
}

function gtcFill(accountId: string, gtc: MockWorkingOrder): Json {
  return fillJson({
    account_id: accountId,
    client_order_id: gtc.clientOrderId,
    source: gtc.source,
    fill_id: `${gtc.clientOrderId}:fill:0`,
    price: '336',
    quantity: '4',
    notional: '1344',
    fee: '0.35',
    filled_at: GTC_FILLED_AT,
  });
}

function gtcEvents(gtc: MockWorkingOrder): Json[] {
  const events = [
    orderEventJson({ sequence: '1', type: 'order_submitted' }),
    orderEventJson({ sequence: '2', type: 'order_accepted' }),
    orderEventJson({ sequence: '3', type: 'account_state_updated' }),
  ];
  if (gtc.stage !== 'working') {
    events.push(
      orderEventJson({
        sequence: '4',
        type: 'fill_applied',
        occurred_at: GTC_FILLED_AT,
        fill: {
          fill_id: `${gtc.clientOrderId}:fill:0`,
          side: 'buy',
          price: '336',
          quantity: '4',
          notional: '1344',
          fee: '0.35',
          filled_at: GTC_FILLED_AT,
        },
      }),
      orderEventJson({ sequence: '5', type: 'account_state_updated', occurred_at: GTC_FILLED_AT }),
    );
  }
  if (gtc.stage === 'cancelled') {
    events.push(
      orderEventJson({ sequence: '6', type: 'order_cancelled', occurred_at: '2026-09-25T13:40:01.000000000Z' }),
      orderEventJson({ sequence: '7', type: 'account_state_updated', occurred_at: '2026-09-25T13:40:01.000000000Z' }),
    );
  }
  return events;
}

// The account while its one GTC order works, fills 4 at 336 with a 0.35
// fee, and has its remainder cancelled. Reservation 336 × 10 + 0.35, then
// 336 × 6, then nothing; the 4 shares are marked at the 335.82 bid. Worked
// out by hand; the mock does no arithmetic.
function gtcResources(state: MockAccount, gtc: MockWorkingOrder): { account: Json; portfolio: Json } {
  const version = String(state.orders.length + (gtc.stage === 'working' ? 0 : 1) + (gtc.stage === 'cancelled' ? 1 : 0));
  if (gtc.stage === 'working') {
    const reservation = { reserved_cash: '3360.35', available_cash: '96639.65' };
    return {
      account: flatAccountJson({ account_id: state.accountId, version, market_stream_id: portfolioJson().market_stream_id, working_order_count: '1', ...reservation }),
      portfolio: flatPortfolioJson({ account_id: state.accountId, account_version: version, market_stream_id: portfolioJson().market_stream_id, ...reservation }),
    };
  }
  const position = { quantity: '4', open_cost_basis: '1344', average_entry_price: '336', gross_realized_pnl: '0', fees_paid: '0.35' };
  const reservation =
    gtc.stage === 'partially_filled'
      ? { reserved_cash: '2016', available_cash: '96639.65', working_order_count: '1' }
      : { reserved_cash: '0', available_cash: '98655.65', working_order_count: '0' };
  const { working_order_count, ...cash } = reservation;
  const ledger = {
    account_id: state.accountId,
    cash: '98655.65',
    open_cost_basis: '1344',
    gross_realized_pnl: '0',
    fees_paid: '0.35',
    last_fill_at: GTC_FILLED_AT,
    market_stream_id: portfolioJson().market_stream_id,
  };
  return {
    account: flatAccountJson({
      ...ledger,
      ...cash,
      version,
      working_order_count,
      positions: [openPositionJson({ ...position, last_fill_at: GTC_FILLED_AT })],
    }),
    portfolio: portfolioJson({
      ...ledger,
      ...cash,
      account_version: version,
      market_value: '1343.28',
      unrealized_pnl: '-0.72',
      total_pnl: '-1.07',
      equity: '99998.93',
      return_percent: '-0.00107',
      positions: [
        valuedPositionJson({ ...position, last_fill_at: GTC_FILLED_AT, mark_price: '335.82', market_value: '1343.28', unrealized_pnl: '-0.72' }),
      ],
    }),
  };
}

function resources(state: MockAccount): { account: Json; portfolio: Json } {
  if (state.gtc !== null) {
    return gtcResources(state, state.gtc);
  }
  const key = [state.holds.aapl && 'aapl', state.holds.spy && 'spy'].filter(Boolean).join('+');
  const version = String(state.orders.length);
  if (key === '') {
    return {
      account: flatAccountJson({ account_id: state.accountId }),
      portfolio: flatPortfolioJson({ account_id: state.accountId }),
    };
  }
  const totals = TOTALS[key] ?? {};
  const { market_value, unrealized_pnl, total_pnl, equity, return_percent, ...ledger } = totals;
  const open = [
    ...(state.holds.aapl ? [openPositionJson(AAPL_POSITION)] : []),
    ...(state.holds.spy ? [openPositionJson(SPY_POSITION)] : []),
  ];
  const valued = [
    ...(state.holds.aapl ? [valuedPositionJson({ ...AAPL_POSITION, ...AAPL_MARK })] : []),
    ...(state.holds.spy ? [valuedPositionJson({ ...SPY_POSITION, ...SPY_MARK })] : []),
  ];
  const common = { account_id: state.accountId, gross_realized_pnl: '0', last_fill_at: MARKET_TIME, ...ledger };
  return {
    account: flatAccountJson({ ...common, version, market_stream_id: portfolioJson().market_stream_id, positions: open }),
    portfolio: portfolioJson({
      ...common,
      account_version: version,
      market_value,
      unrealized_pnl,
      total_pnl,
      equity,
      return_percent,
      positions: valued,
    }),
  };
}

function recordedOrder(accountId: string, body: { client_order_id: string; source: Json; instrument: string }): Json {
  const order = partialOrderJson({ client_order_id: body.client_order_id, source: body.source, account_id: accountId });
  if (body.instrument !== 'SPY') {
    return order;
  }
  return {
    ...order,
    instrument: 'SPY',
    quantity: '2',
    status: 'filled',
    filled_quantity: '2',
    remaining_quantity: '0',
    unfilled_quantity: '0',
    filled_notional: '1539.56',
    cancellation_reason: null,
    fill: {
      fill_id: `${body.client_order_id}:fill:0`,
      side: 'buy',
      price: '769.78',
      quantity: '2',
      notional: '1539.56',
      fee: '0.35',
      filled_at: MARKET_TIME,
    },
    market: spyMarketJson(),
  };
}

function reply(route: Route, status: number, body: unknown) {
  return route.fulfill({
    status,
    contentType: 'application/json',
    headers: { 'X-Chronos-Trading-Mode': 'paper', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' },
    body: JSON.stringify(body),
  });
}

const notFound = errorJson('account_not_found', 'paper account does not exist', { field: 'account_id' });

// Commands the mock replay accepts, and the state each moves to.
const REPLAY_TRANSITIONS: Readonly<Record<string, { from: string; to: string }>> = {
  start: { from: 'ready', to: 'running' },
  pause: { from: 'running', to: 'paused' },
  resume: { from: 'paused', to: 'running' },
};

export async function mockChronosApi(page: Page, options: MockOptions = {}) {
  const accounts = new Map<string, MockAccount>();
  let submissions = 0;
  let replayState = 'ready';
  let revision = 0;
  // Advances when an account records an order, a fill, or a cancellation.
  let activity = 0;
  let liveState = 'live';
  const marketData = () =>
    options.liveSource === true
      ? marketDataJson({
          source: liveSourceJson({
            state: liveState,
            diagnostic:
              liveState === 'reconnecting'
                ? 'the connection closed: the WebSocket connection closed; reconnecting in 1.000 s'
                : null,
          }),
          revision: String(revision),
          reasons:
            liveState === 'live' ? { AAPL: null, SPY: null } : { AAPL: 'source_unavailable', SPY: 'source_unavailable' },
        })
      : marketDataJson({ source: replaySourceJson({ state: replayState }), revision: String(revision) });

  await page.route('**/health', (route) => reply(route, 200, healthJson()));
  await page.route('**/api/v1/market-data**', async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path === '/api/v1/market-data/events') {
      // One notice per connection; the browser reconnects after the retry delay.
      return route.fulfill({
        status: 200,
        contentType: 'text/event-stream',
        headers: { 'Cache-Control': 'no-store', 'X-Chronos-Trading-Mode': 'paper' },
        body: `retry: 3000\n\nevent: market-data\ndata: {"revision":"${String(revision)}","activity_revision":"${String(activity)}"}\n\n`,
      });
    }
    const command = path.split('/').at(-1) ?? '';
    const transition = REPLAY_TRANSITIONS[command];
    if (path === '/api/v1/market-data') {
      return reply(route, 200, { market_data: marketData() });
    }
    if (transition === undefined) {
      return reply(route, 404, errorJson('route_not_found', 'no such API route'));
    }
    if (transition.from !== replayState) {
      return reply(
        route,
        409,
        errorJson('invalid_replay_transition', `cannot ${command} a replay that is ${replayState}`, {
          reason: replayState,
        }),
      );
    }
    replayState = transition.to;
    revision += 1;
    return reply(route, 200, { market_data: marketData() });
  });
  await page.route('**/api/v1/accounts**', async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const [accountId, resource, clientOrderId, orderResource] = url.pathname.split('/').slice(4);
    const method = request.method();
    // History filtered to one symbol, as the server filters it.
    const symbol = url.searchParams.get('instrument');
    const bySymbol = (items: Json[]) => (symbol === null ? items : items.filter((item) => item.instrument === symbol));

    if (accountId === undefined) {
      const body = request.postDataJSON() as { account_id: string; starting_cash: string };
      if (accounts.has(body.account_id)) {
        return reply(route, 409, errorJson('account_already_exists', 'paper account already exists'));
      }
      const state: MockAccount = {
        accountId: body.account_id,
        holds: { aapl: false, spy: false },
        orders: [],
        fills: [],
        gtc: null,
      };
      accounts.set(body.account_id, state);
      return reply(route, 201, { account: resources(state).account });
    }

    const state = accounts.get(accountId);
    if (state === undefined) {
      return reply(route, 404, notFound);
    }
    if (resource === undefined) {
      return reply(route, 200, { account: resources(state).account });
    }
    if (resource === 'portfolio') {
      return reply(route, 200, { portfolio: resources(state).portfolio });
    }
    if (resource === 'fills') {
      return reply(route, 200, { fills: bySymbol(state.fills), next_cursor: null });
    }
    if (resource === 'working-orders') {
      const working = state.gtc !== null && state.gtc.stage !== 'cancelled' ? [gtcOrder(accountId, state.gtc)] : [];
      return reply(route, 200, { orders: working, next_cursor: null });
    }
    const gtc = state.gtc;
    if (gtc !== null && clientOrderId === gtc.clientOrderId && orderResource === 'events') {
      return reply(route, 200, { events: gtcEvents(gtc), next_cursor: null });
    }
    if (gtc !== null && clientOrderId === gtc.clientOrderId && orderResource === 'cancel' && method === 'POST') {
      const { cancel_request_id: requestId } = request.postDataJSON() as { cancel_request_id: string };
      if (gtc.stage === 'cancelled') {
        return requestId === gtc.cancelRequestId
          ? reply(route, 200, { order: gtcOrder(accountId, gtc), account: resources(state).account, already_cancelled: true })
          : reply(route, 409, {
              ...errorJson('order_not_cancellable', 'the order has no working remainder to cancel', { field: 'client_order_id' }),
              order: gtcOrder(accountId, gtc),
            });
      }
      gtc.stage = 'cancelled';
      gtc.cancelRequestId = requestId;
      state.orders = state.orders.map((order) => (order.client_order_id === gtc.clientOrderId ? gtcOrder(accountId, gtc) : order));
      activity += 1;
      revision += 1;
      return reply(route, 200, { order: gtcOrder(accountId, gtc), account: resources(state).account, already_cancelled: false });
    }
    if (method === 'GET' && clientOrderId !== undefined) {
      const order = state.orders.find((candidate) => candidate.client_order_id === clientOrderId);
      return order === undefined
        ? reply(route, 404, errorJson('order_not_found', 'paper order does not exist', { field: 'client_order_id' }))
        : reply(route, 200, { order });
    }
    if (method === 'GET') {
      return reply(route, 200, { orders: bySymbol(state.orders), next_cursor: null });
    }

    submissions += 1;
    const body = request.postDataJSON() as {
      client_order_id: string;
      source: Json;
      instrument: string;
      time_in_force: string;
    };
    if (body.time_in_force === 'gtc') {
      state.gtc = { clientOrderId: body.client_order_id, source: body.source, stage: 'working', cancelRequestId: null };
      const order = gtcOrder(accountId, state.gtc);
      state.orders.unshift(order);
      activity += 1;
      return reply(route, 201, { order, account: resources(state).account });
    }
    const order = recordedOrder(accountId, body);
    const fill = order.fill as Json;
    state.orders.unshift(order);
    state.fills.unshift(
      fillJson({
        account_id: accountId,
        client_order_id: body.client_order_id,
        instrument: order.instrument,
        fill_id: fill.fill_id,
        price: fill.price,
        quantity: fill.quantity,
        notional: fill.notional,
      }),
    );
    if (body.instrument === 'SPY') {
      state.holds.spy = true;
    } else {
      state.holds.aapl = true;
    }
    if (options.loseFirstSubmissionResponse === true && submissions === 1) {
      return route.abort('failed');
    }
    return reply(route, 201, { order, account: resources(state).account });
  });

  return {
    submissionCount: () => submissions,
    // A later ask of 336 × 4 reaches the account's working order.
    fillWorkingOrder: (accountId: string) => {
      const state = accounts.get(accountId);
      const gtc = state?.gtc;
      if (state === undefined || gtc?.stage !== 'working') {
        throw new Error(`no working order to fill for ${accountId}`);
      }
      gtc.stage = 'partially_filled';
      state.orders = state.orders.map((order) => (order.client_order_id === gtc.clientOrderId ? gtcOrder(accountId, gtc) : order));
      state.fills.unshift(gtcFill(accountId, gtc));
      state.holds.aapl = true;
      activity += 1;
      revision += 1;
    },
    // Moves the mock live source, as a dropped or restored feed would.
    setLiveState: (state: string) => {
      liveState = state;
      revision += 1;
    },
  };
}
