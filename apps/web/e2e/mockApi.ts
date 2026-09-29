// A small stateful stand-in for the C++ API, answered inside the browser.
// Every body is contract JSON from the shared fixtures, and every response
// carries the headers the real server sends.
import type { Page, Route } from '@playwright/test';

import type { Json } from '../src/test/fixtures';
import {
  errorJson,
  fillJson,
  flatAccountJson,
  healthJson,
  partialOrderJson,
  portfolioJson,
} from '../src/test/fixtures';

interface MockAccount {
  account: Json;
  portfolio: Json;
  orders: Json[];
  fills: Json[];
}

export interface MockOptions {
  // Fail the first submission in transit even though it was recorded, as
  // when a response is lost after the commit.
  readonly loseFirstSubmissionResponse?: boolean;
}

// The account after the documented 10-share market buy filled 4 at 337 with
// a 0.35 fee, marked at the 335.82 bid (values the real server returned).
function afterPartialBuy(accountId: string): Pick<MockAccount, 'account' | 'portfolio'> {
  const ledger = {
    account_id: accountId,
    cash: '98651.65',
    position: '4',
    open_cost_basis: '1348',
    average_entry_price: '337',
    gross_realized_pnl: '0',
    fees_paid: '0.35',
  };
  return {
    account: flatAccountJson({ ...ledger, version: '1', last_fill_at: '2026-09-25T13:39:59.969574414Z' }),
    portfolio: portfolioJson({
      ...ledger,
      account_version: '1',
      position_market_value: '1343.28',
      unrealized_pnl: '-4.72',
      total_pnl: '-5.07',
      equity: '99994.93',
      return_percent: '-0.00507',
    }),
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

export async function mockChronosApi(page: Page, options: MockOptions = {}) {
  const accounts = new Map<string, MockAccount>();
  let submissions = 0;

  await page.route('**/health', (route) => reply(route, 200, healthJson()));
  await page.route('**/api/v1/accounts**', async (route) => {
    const request = route.request();
    const [accountId, resource, clientOrderId] = new URL(request.url()).pathname.split('/').slice(4);
    const method = request.method();

    if (accountId === undefined) {
      const body = request.postDataJSON() as { account_id: string; starting_cash: string };
      if (accounts.has(body.account_id)) {
        return reply(route, 409, errorJson('account_already_exists', 'paper account already exists'));
      }
      const account = flatAccountJson({ account_id: body.account_id });
      accounts.set(body.account_id, {
        account,
        portfolio: portfolioJson({
          account_id: body.account_id,
          account_version: '0',
          cash: '100000',
          position: '0',
          open_cost_basis: '0',
          average_entry_price: null,
          gross_realized_pnl: '0',
          fees_paid: '0',
          position_market_value: '0',
          unrealized_pnl: '0',
          total_pnl: '0',
          equity: '100000',
          return_percent: '0',
        }),
        orders: [],
        fills: [],
      });
      return reply(route, 201, { account });
    }

    const state = accounts.get(accountId);
    if (state === undefined) {
      return reply(route, 404, notFound);
    }
    if (resource === undefined) {
      return reply(route, 200, { account: state.account });
    }
    if (resource === 'portfolio') {
      return reply(route, 200, { portfolio: state.portfolio });
    }
    if (resource === 'fills') {
      return reply(route, 200, { fills: state.fills, next_cursor: null });
    }
    if (method === 'GET' && clientOrderId !== undefined) {
      const order = state.orders.find((candidate) => candidate.client_order_id === clientOrderId);
      return order === undefined
        ? reply(route, 404, errorJson('order_not_found', 'paper order does not exist', { field: 'client_order_id' }))
        : reply(route, 200, { order });
    }
    if (method === 'GET') {
      return reply(route, 200, { orders: state.orders, next_cursor: null });
    }

    // Every submission is the documented partial fill of a 10-share market buy.
    submissions += 1;
    const body = request.postDataJSON() as { client_order_id: string; source: Json };
    const order = partialOrderJson({ client_order_id: body.client_order_id, source: body.source, account_id: accountId });
    state.orders.unshift(order);
    state.fills.unshift(
      fillJson({ account_id: accountId, client_order_id: body.client_order_id, fill_id: `${body.client_order_id}:fill:0` }),
    );
    Object.assign(state, afterPartialBuy(accountId));
    if (options.loseFirstSubmissionResponse === true && submissions === 1) {
      return route.abort('failed');
    }
    return reply(route, 201, { order, account: state.account });
  });

  return {
    submissionCount: () => submissions,
  };
}
