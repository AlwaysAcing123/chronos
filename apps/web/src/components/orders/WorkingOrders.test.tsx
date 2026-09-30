import { screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { App } from '../../App';
import { activityRevisionOf } from '../../state/marketDataStream';
import type { FakeApi } from '../../test/fakeApi';
import { createFakeApi } from '../../test/fakeApi';
import { fakeNoticeStreams } from '../../test/fakeNoticeStream';
import {
  accountJson,
  errorJson,
  orderEventJson,
  partiallyFilledWorkingOrderJson,
  PERSISTENCE_UNAVAILABLE_MESSAGE,
  userCancelledOrderJson,
  workingOrderJson,
} from '../../test/fixtures';
import { accountPath, rememberSelectedAccount, renderWithServices, serveAccount } from '../../test/renderWithServices';

const workingPath = `${accountPath}/working-orders`;
const cancelPath = `${accountPath}/orders/gtc-1/cancel`;
const orderPath = `${accountPath}/orders/gtc-1`;

async function openWorkingOrders(api: FakeApi, working = [workingOrderJson()]) {
  rememberSelectedAccount();
  serveAccount(api, { workingOrders: working });
  const rendered = renderWithServices(<App />, { api });
  const panel = await screen.findByRole('region', { name: 'Working orders' });
  return { ...rendered, panel };
}

function cancelBodies(api: FakeApi): unknown[] {
  return api.requestsTo('POST', cancelPath).map((request) => request.body);
}

function rowCells(panel: HTMLElement, key: string): (string | null)[] {
  const table = within(panel).getByRole('region', { name: 'Working order list' });
  const row = within(table)
    .getAllByRole('row')
    .find((candidate) => within(candidate).queryAllByRole('cell')[1]?.textContent === key);
  if (row === undefined) {
    throw new Error(`no row for ${key}`);
  }
  return within(row).getAllByRole('cell').map((cell) => cell.textContent);
}

describe('working orders', () => {
  it('lists each working order with the server quantities and reservation', async () => {
    const api = createFakeApi();
    const { panel } = await openWorkingOrders(api, [workingOrderJson(), partiallyFilledWorkingOrderJson({ client_order_id: 'gtc-2', priority: '42' })]);
    await within(panel).findByText('gtc-2');
    expect(rowCells(panel, 'gtc-1').slice(2, 11)).toEqual([
      'AAPL', 'Buy', '336', '10', '0', '10', 'Working', 'Manualweb-ticket', '3360.35cash · incl. fee',
    ]);
    expect(rowCells(panel, 'gtc-2').slice(6, 11)).toEqual([
      '4', '6', 'Working, partially filled', 'Manualweb-ticket', '2016cash',
    ]);
    expect(within(panel).getByText('2 working orders loaded · in priority order')).toBeInTheDocument();
  });

  it('cancels the remainder and shows what was released', async () => {
    const api = createFakeApi();
    const { panel, user } = await openWorkingOrders(api, [partiallyFilledWorkingOrderJson()]);
    api.on('POST', cancelPath, (request) => {
      api.on('GET', workingPath, { status: 200, body: { orders: [], next_cursor: null } });
      return {
        status: 200,
        body: {
          order: userCancelledOrderJson({
            cancel_request_id: (request.body as { cancel_request_id: string }).cancel_request_id,
          }),
          account: accountJson(),
          already_cancelled: false,
        },
      };
    });
    await user.click(await within(panel).findByRole('button', { name: 'Cancel remainder of gtc-1' }));

    expect(await within(panel).findByText('gtc-1: remainder cancelled')).toBeInTheDocument();
    expect(within(panel).getByText('4 of 10 filled; 6 cancelled and released.')).toBeInTheDocument();
    expect(await within(panel).findByText(/No working orders/)).toBeInTheDocument();
    const [body] = cancelBodies(api) as { cancel_request_id: string }[];
    expect(body?.cancel_request_id).toMatch(/^cancel-[0-9a-z]+-[0-9a-z]{10}$/);
  });

  it('reconciles an uncertain cancel and retries it with the same ID', async () => {
    const api = createFakeApi();
    const { panel, user } = await openWorkingOrders(api);
    api.on('POST', cancelPath, { status: 503, body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE) });
    api.on('GET', orderPath, { status: 200, body: { order: workingOrderJson() } });
    await user.click(await within(panel).findByRole('button', { name: 'Cancel remainder of gtc-1' }));

    expect(await within(panel).findByText('gtc-1 is still working')).toBeInTheDocument();
    expect(api.requestsTo('GET', orderPath)).toHaveLength(1);
    api.on('POST', cancelPath, (request) => ({
      status: 200,
      body: {
        order: userCancelledOrderJson({ cancel_request_id: (request.body as { cancel_request_id: string }).cancel_request_id }),
        account: accountJson(),
        already_cancelled: false,
      },
    }));
    await user.click(within(panel).getByRole('button', { name: 'Send the same cancel again' }));
    expect(await within(panel).findByText('gtc-1: remainder cancelled')).toBeInTheDocument();
    const [first, second] = cancelBodies(api) as { cancel_request_id: string }[];
    expect(second?.cancel_request_id).toBe(first?.cancel_request_id);
  });

  it('learns from a lookup that an uncertain cancel succeeded', async () => {
    const api = createFakeApi();
    const { panel, user } = await openWorkingOrders(api);
    api.failNext('POST', cancelPath);
    api.on('POST', cancelPath, { status: 500, body: errorJson('internal_error', 'unexpected') });
    api.on('GET', orderPath, () => {
      const [body] = cancelBodies(api) as { cancel_request_id: string }[];
      return { status: 200, body: { order: userCancelledOrderJson({ cancel_request_id: body?.cancel_request_id ?? '' }) } };
    });
    await user.click(await within(panel).findByRole('button', { name: 'Cancel remainder of gtc-1' }));
    expect(await within(panel).findByText('gtc-1: remainder cancelled')).toBeInTheDocument();
    expect(within(panel).getByText(/Confirmed by reading the order/)).toBeInTheDocument();
    expect(cancelBodies(api)).toHaveLength(1);
  });

  it('keeps the outcome unknown while the lookup fails, and never invents a new ID', async () => {
    const api = createFakeApi();
    const { panel, user } = await openWorkingOrders(api);
    api.on('POST', cancelPath, { status: 503, body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE) });
    api.on('GET', orderPath, { status: 503, body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE) });
    await user.click(await within(panel).findByRole('button', { name: 'Cancel remainder of gtc-1' }));
    expect(await within(panel).findByText('Cancel of gtc-1: outcome still unknown')).toBeInTheDocument();
    await user.click(within(panel).getByRole('button', { name: 'Check again' }));
    await waitFor(() => {
      expect(api.requestsTo('GET', orderPath)).toHaveLength(2);
    });
    await user.click(await within(panel).findByRole('button', { name: 'Retry cancel of gtc-1' }));
    await waitFor(() => {
      expect(cancelBodies(api)).toHaveLength(2);
    });
    const [first, second] = cancelBodies(api) as { cancel_request_id: string }[];
    expect(second?.cancel_request_id).toBe(first?.cancel_request_id);
  });

  it('shows an order that filled first as having nothing left to cancel', async () => {
    const api = createFakeApi();
    const { panel, user } = await openWorkingOrders(api);
    api.on('POST', cancelPath, {
      status: 409,
      body: {
        ...errorJson('order_not_cancellable', 'the order has no working remainder to cancel', { field: 'client_order_id' }),
        order: workingOrderJson({
          status: 'filled',
          filled_quantity: '10',
          remaining_quantity: '0',
          unfilled_quantity: '0',
          filled_notional: '3360',
          fill_count: '1',
          fee_charged: true,
          cumulative_fee: '0.35',
          reserved_cash: '0',
        }),
      },
    });
    await user.click(await within(panel).findByRole('button', { name: 'Cancel remainder of gtc-1' }));
    expect(await within(panel).findByText('gtc-1: nothing left to cancel')).toBeInTheDocument();
    expect(within(panel).getByText('It is now filled; 10 of 10 filled.')).toBeInTheDocument();
    await user.click(within(panel).getByRole('button', { name: 'Dismiss' }));
    expect(within(panel).queryByText('gtc-1: nothing left to cancel')).not.toBeInTheDocument();
  });

  it('shows an order lifecycle with its fills', async () => {
    const api = createFakeApi();
    const { panel, user } = await openWorkingOrders(api, [partiallyFilledWorkingOrderJson()]);
    api.on('GET', `${orderPath}/events`, {
      status: 200,
      body: {
        events: [
          orderEventJson(),
          orderEventJson({ sequence: '2', type: 'order_accepted' }),
          orderEventJson({ sequence: '3', type: 'account_state_updated' }),
          orderEventJson({
            sequence: '4',
            type: 'fill_applied',
            occurred_at: '2026-09-25T13:40:00.000000000Z',
            fill: {
              fill_id: 'gtc-1:fill:0',
              side: 'buy',
              price: '336',
              quantity: '4',
              notional: '1344',
              fee: '0.35',
              filled_at: '2026-09-25T13:40:00.000000000Z',
            },
          }),
        ],
        next_cursor: null,
      },
    });
    await user.click(await within(panel).findByRole('button', { name: 'Lifecycle of gtc-1' }));
    const events = await within(panel).findByRole('list', { name: 'Events of gtc-1' });
    expect(within(events).getByText('Accepted as a working order')).toBeInTheDocument();
    expect(within(events).getByText(/4 at 336 · notional 1344 · fee 0.35/)).toBeInTheDocument();
  });
});

describe('change notices', () => {
  it('read the activity revision only from the documented notice', () => {
    const notice = (data: unknown) => new MessageEvent('market-data', { data });
    expect(activityRevisionOf(notice('{"revision":"3","activity_revision":"2"}'))).toBe('2');
    expect(activityRevisionOf(notice('{"revision":"3"}'))).toBeNull();
    expect(activityRevisionOf(notice('{"revision":"3","activity_revision":2}'))).toBeNull();
    expect(activityRevisionOf(notice('{"revision":"3","activity_revision":"02"}'))).toBeNull();
    expect(activityRevisionOf(notice('not json'))).toBeNull();
  });

  it('refresh working orders and the account only when account activity changed', async () => {
    const api = createFakeApi();
    rememberSelectedAccount();
    serveAccount(api, { workingOrders: [workingOrderJson()] });
    const notices = fakeNoticeStreams();
    renderWithServices(<App />, { api, openNoticeStream: notices.open });
    const panel = await screen.findByRole('region', { name: 'Working orders' });
    await within(panel).findByText('gtc-1');
    const stream = notices.streams[0];
    if (stream === undefined) {
      throw new Error('no notice stream opened');
    }
    stream.open();
    stream.notice('1', '5');
    await waitFor(() => {
      expect(api.requestsTo('GET', workingPath).length).toBeGreaterThanOrEqual(2);
    });
    const settled = api.requestsTo('GET', workingPath).length;
    // Market-only notices leave durable account resources to their polling.
    stream.notice('2', '5');
    stream.notice('3', '5');
    await waitFor(() => {
      expect(api.requestsTo('GET', '/api/v1/market-data').length).toBeGreaterThanOrEqual(3);
    });
    expect(api.requestsTo('GET', workingPath)).toHaveLength(settled);
    // A fill elsewhere moves the activity revision.
    api.on('GET', workingPath, { status: 200, body: { orders: [partiallyFilledWorkingOrderJson()], next_cursor: null } });
    stream.notice('4', '6');
    expect(await within(panel).findByText('Working, partially filled')).toBeInTheDocument();
  });
});
