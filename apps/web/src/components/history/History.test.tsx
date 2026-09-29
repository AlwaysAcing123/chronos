import { screen, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { App } from '../../App';
import { HISTORY_PAGE_SIZE } from '../../api/client';
import { createFakeApi } from '../../test/fakeApi';
import {
  cancelledOrderJson,
  errorJson,
  fillJson,
  filledOrderJson,
  partialOrderJson,
  PERSISTENCE_UNAVAILABLE_MESSAGE,
  rejectedOrderJson,
} from '../../test/fixtures';
import { accountPath, rememberSelectedAccount, renderWithServices, serveAccount } from '../../test/renderWithServices';

const modelSource = { kind: 'model', id: 'mean-rev-v1' };

async function openHistory(api = createFakeApi()) {
  rememberSelectedAccount();
  const rendered = renderWithServices(<App />, { api });
  const orders = await screen.findByRole('region', { name: 'Orders' });
  const fills = screen.getByRole('region', { name: 'Fills' });
  return { ...rendered, orders, fills };
}

function rowsOf(region: HTMLElement): HTMLElement[] {
  return within(within(region).getByRole('table')).getAllByRole('row').slice(1);
}

// Queries scoped to one body row, which must exist.
function row(region: HTMLElement, index: number) {
  const found = rowsOf(region)[index];
  if (found === undefined) {
    throw new Error(`row ${String(index)} is missing`);
  }
  return within(found);
}

describe('order blotter', () => {
  it('shows every recorded outcome with its source and reason', async () => {
    const api = createFakeApi();
    serveAccount(api, {
      orders: [
        rejectedOrderJson({ source: modelSource }),
        cancelledOrderJson(),
        filledOrderJson({ source: modelSource }),
        partialOrderJson(),
      ],
    });
    const { orders } = await openHistory(api);
    await within(orders).findByText('buy-big');
    const rejected = row(orders, 0);
    const cancelled = row(orders, 1);
    const filled = row(orders, 2);
    const partial = row(orders, 3);

    expect(rejected.getByText('Rejected')).toBeInTheDocument();
    expect(rejected.getByText('insufficient_cash')).toBeInTheDocument();
    expect(rejected.getByText('Model')).toBeInTheDocument();
    expect(rejected.getByText('mean-rev-v1')).toBeInTheDocument();
    expect(cancelled.getByText('Cancelled')).toBeInTheDocument();
    expect(cancelled.getByText('Limit price was not marketable against the displayed quote')).toBeInTheDocument();
    expect(cancelled.getByText('Manual')).toBeInTheDocument();
    expect(filled.getByText('Filled')).toBeInTheDocument();
    expect(filled.getByText('335.82')).toBeInTheDocument();
    expect(partial.getByText('Partially filled')).toBeInTheDocument();
    expect(partial.getByText('13:39:59.969574414')).toBeInTheDocument();
    expect(within(orders).getByText('Oldest reached')).toBeInTheDocument();
  });

  it('loads older orders with the server cursor and appends them', async () => {
    const api = createFakeApi();
    serveAccount(api);
    api.on('GET', `${accountPath}/orders`, (request) =>
      request.query.get('cursor') === 'djF8b3JkZXJzfGRlbW8tMXwx'
        ? { status: 200, body: { orders: [cancelledOrderJson({ client_order_id: 'oldest' })], next_cursor: null } }
        : { status: 200, body: { orders: [partialOrderJson()], next_cursor: 'djF8b3JkZXJzfGRlbW8tMXwx' } },
    );
    const { orders, user } = await openHistory(api);
    await within(orders).findByText('buy-1');
    expect(within(orders).queryByText('Oldest reached')).not.toBeInTheDocument();

    await user.click(within(orders).getByRole('button', { name: 'Load older orders' }));
    expect(await within(orders).findByText('oldest')).toBeInTheDocument();
    expect(rowsOf(orders).map((row) => row.querySelector('td:nth-child(2)')?.textContent)).toEqual(['buy-1', 'oldest']);
    expect(within(orders).getByText('Oldest reached')).toBeInTheDocument();

    const requests = api.requestsTo('GET', `${accountPath}/orders`);
    expect(requests.every((request) => request.query.get('limit') === String(HISTORY_PAGE_SIZE))).toBe(true);
    expect(requests.some((request) => request.query.get('cursor') === 'djF8b3JkZXJzfGRlbW8tMXwx')).toBe(true);
    expect(requests.some((request) => request.query.has('offset'))).toBe(false);
  });

  it('explains an empty history', async () => {
    const api = createFakeApi();
    serveAccount(api, { orders: [], fills: [] });
    const { orders, fills } = await openHistory(api);
    expect(await within(orders).findByText('No orders recorded for this account yet.')).toBeInTheDocument();
    expect(await within(fills).findByText(/No fills booked yet/)).toBeInTheDocument();
  });

  it('offers a retry when the first read fails', async () => {
    const api = createFakeApi();
    serveAccount(api);
    let failing = true;
    api.on('GET', `${accountPath}/orders`, () =>
      failing
        ? { status: 503, body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE) }
        : { status: 200, body: { orders: [partialOrderJson()], next_cursor: null } },
    );
    const { orders, user } = await openHistory(api);
    expect(await within(orders).findByText('The paper-trading database is unavailable')).toBeInTheDocument();
    failing = false;
    await user.click(within(orders).getByRole('button', { name: 'Retry' }));
    expect(await within(orders).findByText('buy-1')).toBeInTheDocument();
  });

  it('shows a loading state until the first page arrives', async () => {
    const api = createFakeApi();
    serveAccount(api);
    api.on('GET', `${accountPath}/orders`, () => new Promise(() => undefined));
    const { orders } = await openHistory(api);
    expect(within(orders).getByRole('status', { name: 'Loading orders' })).toBeInTheDocument();
  });
});

describe('fill history', () => {
  it('lists booked fills with exact values and their source', async () => {
    const api = createFakeApi();
    serveAccount(api, {
      fills: [
        fillJson({ client_order_id: 'model-sell-1', fill_id: 'model-sell-1:fill:0', source: modelSource, side: 'sell' }),
        fillJson(),
      ],
    });
    const { fills } = await openHistory(api);
    await within(fills).findByText('model-sell-1:fill:0');
    const modelFill = row(fills, 0);
    const manualFill = row(fills, 1);
    expect(modelFill.getByText('Model')).toBeInTheDocument();
    expect(modelFill.getByText('Sell')).toBeInTheDocument();
    expect(manualFill.getByText('Manual')).toBeInTheDocument();
    expect(manualFill.getByText('1348')).toBeInTheDocument();
    expect(manualFill.getByText('0.35')).toBeInTheDocument();
  });
});
