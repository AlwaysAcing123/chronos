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
  partialOrderJson,
  portfolioJson,
  replaySourceJson,
  spyMarketJson,
  valuedPositionJson,
} from '../src/test/fixtures';

interface MockAccount {
  accountId: string;
  // Which canned fills the account has booked.
  holds: { aapl: boolean; spy: boolean };
  orders: Json[];
  fills: Json[];
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

function resources(state: MockAccount): { account: Json; portfolio: Json } {
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
    unfilled_quantity: '0',
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
        body: `retry: 3000\n\nevent: market-data\ndata: {"revision":"${String(revision)}"}\n\n`,
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
    const [accountId, resource, clientOrderId] = url.pathname.split('/').slice(4);
    const method = request.method();
    // History filtered to one symbol, as the server filters it.
    const symbol = url.searchParams.get('instrument');
    const bySymbol = (items: Json[]) => (symbol === null ? items : items.filter((item) => item.instrument === symbol));

    if (accountId === undefined) {
      const body = request.postDataJSON() as { account_id: string; starting_cash: string };
      if (accounts.has(body.account_id)) {
        return reply(route, 409, errorJson('account_already_exists', 'paper account already exists'));
      }
      const state: MockAccount = { accountId: body.account_id, holds: { aapl: false, spy: false }, orders: [], fills: [] };
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
    const body = request.postDataJSON() as { client_order_id: string; source: Json; instrument: string };
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
    // Moves the mock live source, as a dropped or restored feed would.
    setLiveState: (state: string) => {
      liveState = state;
      revision += 1;
    },
  };
}
