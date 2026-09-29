import { describe, expect, it } from 'vitest';

import { createFakeApi } from '../test/fakeApi';
import {
  ACCOUNT_ID,
  accountJson,
  errorJson,
  fillJson,
  healthJson,
  marketDataJson,
  partialOrderJson,
  PERSISTENCE_UNAVAILABLE_MESSAGE,
} from '../test/fixtures';
import { createChronosClient, HISTORY_PAGE_SIZE } from './client';
import { ChronosApiError, categoryOf } from './errors';

const accountPath = `/api/v1/accounts/${ACCOUNT_ID}`;

async function failureOf(promise: Promise<unknown>): Promise<ChronosApiError> {
  try {
    await promise;
  } catch (error) {
    if (error instanceof ChronosApiError) {
      return error;
    }
    throw error;
  }
  throw new Error('expected the request to fail');
}

describe('ChronosClient', () => {
  it('decodes health with 200 and 503 alike', async () => {
    const api = createFakeApi();
    api.on('GET', '/health', {
      status: 503,
      body: healthJson({ status: 'unavailable', persistence: 'unavailable' }),
    });
    const health = await api.client.getHealth();
    expect(health.persistence).toBe('unavailable');
    expect(health.tradeable_instruments).toEqual(['AAPL', 'SPY']);
  });

  it('reports a portfolio the market cannot value as data, not as a failure', async () => {
    const api = createFakeApi();
    api.on('GET', `${accountPath}/portfolio`, {
      status: 503,
      body: errorJson('market_data_unavailable', 'top of book has no best bid', {
        reason: 'missing_best_bid',
      }),
    });
    await expect(api.client.getPortfolio(ACCOUNT_ID)).resolves.toEqual({
      kind: 'unavailable',
      category: 'market_data_unavailable',
      error: {
        code: 'market_data_unavailable',
        message: 'top of book has no best bid',
        field: null,
        reason: 'missing_best_bid',
      },
    });
  });

  it('distinguishes a persistence outage from unusable market data', async () => {
    const api = createFakeApi();
    api.on('GET', `${accountPath}/portfolio`, {
      status: 503,
      body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE),
    });
    const failure = await failureOf(api.client.getPortfolio(ACCOUNT_ID));
    expect(categoryOf(failure)).toBe('persistence_unavailable');
  });

  it('classifies documented errors by code', async () => {
    const api = createFakeApi();
    api.on('GET', accountPath, {
      status: 404,
      body: errorJson('account_not_found', 'paper account does not exist', { field: 'account_id' }),
    });
    expect(categoryOf(await failureOf(api.client.getAccount(ACCOUNT_ID)))).toBe('account_not_found');
  });

  it('treats an unknown error code as an unexpected response', async () => {
    const api = createFakeApi();
    api.on('GET', accountPath, { status: 418, body: errorJson('teapot', 'no') });
    expect(categoryOf(await failureOf(api.client.getAccount(ACCOUNT_ID)))).toBe('unexpected_response');
  });

  it('refuses a success body that breaks the contract', async () => {
    const api = createFakeApi();
    api.on('GET', accountPath, { status: 200, body: { account: accountJson({ cash: 99155.03 }) } });
    const failure = await failureOf(api.client.getAccount(ACCOUNT_ID));
    expect(failure.failure).toMatchObject({ kind: 'unexpected_response', status: 200 });
    expect(failure.message).toContain('account.cash');
  });

  it('refuses a body that is not JSON', async () => {
    const api = createFakeApi();
    api.on('GET', accountPath, { status: 200, rawBody: '<html>proxy error</html>' });
    const failure = await failureOf(api.client.getAccount(ACCOUNT_ID));
    expect(failure.failure).toMatchObject({ kind: 'unexpected_response', detail: 'the response body is not JSON' });
  });

  it('refuses a response that did not come from the Chronos API', async () => {
    const api = createFakeApi();
    api.on('GET', accountPath, { status: 200, body: { account: accountJson() }, omitTradingModeHeader: true });
    const failure = await failureOf(api.client.getAccount(ACCOUNT_ID));
    expect(failure.failure).toMatchObject({ kind: 'unexpected_response' });
  });

  it('reports a proxy that could not reach the server as a network failure', async () => {
    const api = createFakeApi();
    api.on('GET', accountPath, { status: 502, rawBody: '', omitTradingModeHeader: true });
    const failure = await failureOf(api.client.getAccount(ACCOUNT_ID));
    expect(failure.failure).toEqual({ kind: 'network', detail: 'the proxy could not reach the Chronos API' });
  });

  it('reports a request that never reached the server as a network failure', async () => {
    const api = createFakeApi();
    api.failNext('GET', accountPath);
    expect(categoryOf(await failureOf(api.client.getAccount(ACCOUNT_ID)))).toBe('network');
  });

  it('reports a hung server as a network failure after the timeout', async () => {
    const hung: typeof fetch = (_input, init) =>
      new Promise((_resolve, reject) => {
        init?.signal?.addEventListener('abort', () => {
          reject(new DOMException('aborted', 'AbortError'));
        });
      });
    const client = createChronosClient({ fetch: hung, readTimeoutMs: 20, writeTimeoutMs: 20 });
    const failure = await failureOf(client.getAccount(ACCOUNT_ID));
    expect(failure.failure).toEqual({ kind: 'network', detail: 'the Chronos API did not respond within 0.02 s' });
  });

  it('passes a caller cancellation through unchanged', async () => {
    const api = createFakeApi();
    api.on('GET', accountPath, () => new Promise(() => undefined));
    const controller = new AbortController();
    const reason = new DOMException('cancelled by the query', 'AbortError');
    const pending = api.client.getAccount(ACCOUNT_ID, controller.signal);
    controller.abort(reason);
    await expect(pending).rejects.toBe(reason);
  });

  it('pages history with the server cursor and a fixed page size', async () => {
    const api = createFakeApi();
    api.on('GET', `${accountPath}/fills`, (request) => ({
      status: 200,
      body: request.query.has('cursor')
        ? { fills: [fillJson({ fill_id: 'older:fill:0' })], next_cursor: null }
        : { fills: [fillJson()], next_cursor: 'djF8ZmlsbHN8ZGVtby0xfDQy' },
    }));
    const first = await api.client.listFills(ACCOUNT_ID, null, null);
    const older = await api.client.listFills(ACCOUNT_ID, null, first.next_cursor);
    expect(older.fills[0]?.fill_id).toBe('older:fill:0');
    const [firstRequest, olderRequest] = api.requestsTo('GET', `${accountPath}/fills`);
    expect(firstRequest?.query.get('limit')).toBe(String(HISTORY_PAGE_SIZE));
    expect(firstRequest?.query.has('cursor')).toBe(false);
    expect(firstRequest?.query.has('instrument')).toBe(false);
    expect(olderRequest?.query.get('cursor')).toBe('djF8ZmlsbHN8ZGVtby0xfDQy');
  });

  it('filters history to one symbol and keeps the filter on every page', async () => {
    const api = createFakeApi();
    api.on('GET', `${accountPath}/orders`, (request) => ({
      status: 200,
      body: request.query.has('cursor')
        ? { orders: [partialOrderJson({ client_order_id: 'older', instrument: 'SPY' })], next_cursor: null }
        : { orders: [partialOrderJson({ instrument: 'SPY' })], next_cursor: 'djJ8b3JkZXJzfGRlbW8tMXxTUFl8NDI' },
    }));
    const first = await api.client.listOrders(ACCOUNT_ID, 'SPY', null);
    await api.client.listOrders(ACCOUNT_ID, 'SPY', first.next_cursor);
    const [firstRequest, olderRequest] = api.requestsTo('GET', `${accountPath}/orders`);
    expect(firstRequest?.query.get('instrument')).toBe('SPY');
    expect(olderRequest?.query.get('instrument')).toBe('SPY');
    expect(olderRequest?.query.get('cursor')).toBe('djJ8b3JkZXJzfGRlbW8tMXxTUFl8NDI');
  });

  it('looks up an order and reports an absent one as not recorded', async () => {
    const api = createFakeApi();
    api.on('GET', `${accountPath}/orders/buy-1`, { status: 200, body: { order: partialOrderJson() } });
    api.on('GET', `${accountPath}/orders/missing`, {
      status: 404,
      body: errorJson('order_not_found', 'paper order does not exist', { field: 'client_order_id' }),
    });
    await expect(api.client.lookupOrder(ACCOUNT_ID, 'buy-1')).resolves.toMatchObject({ kind: 'recorded' });
    await expect(api.client.lookupOrder(ACCOUNT_ID, 'missing')).resolves.toEqual({ kind: 'not_recorded' });
  });

  it('does not mistake a missing account for a missing order', async () => {
    const api = createFakeApi();
    api.on('GET', `${accountPath}/orders/buy-1`, {
      status: 404,
      body: errorJson('account_not_found', 'paper account does not exist', { field: 'account_id' }),
    });
    const failure = await failureOf(api.client.lookupOrder(ACCOUNT_ID, 'buy-1'));
    expect(categoryOf(failure)).toBe('account_not_found');
  });

  it('sends account creation exactly as entered', async () => {
    const api = createFakeApi();
    api.on('POST', '/api/v1/accounts', { status: 201, body: { account: accountJson() } });
    await api.client.createAccount({ account_id: ACCOUNT_ID, starting_cash: '100000.00' });
    // An account is not created for an instrument, so none is sent.
    expect(api.requestsTo('POST', '/api/v1/accounts')[0]?.body).toEqual({
      account_id: ACCOUNT_ID,
      starting_cash: '100000.00',
    });
  });

  it('sends replay commands as empty objects and types their refusals', async () => {
    const api = createFakeApi();
    api.on('POST', '/api/v1/market-data/replay/start', { status: 200, body: { market_data: marketDataJson() } });
    api.on('POST', '/api/v1/market-data/replay/pause', {
      status: 409,
      body: errorJson('replay_control_unavailable', 'the configured source is not a replay', { reason: 'live' }),
    });
    const started = await api.client.controlReplay('start');
    expect(started.source.state).toBe('running');
    expect(api.requestsTo('POST', '/api/v1/market-data/replay/start')[0]?.body).toEqual({});
    expect(categoryOf(await failureOf(api.client.controlReplay('pause')))).toBe('replay_control_unavailable');
  });
});
