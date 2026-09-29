import { screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { App } from '../../App';
import type { FakeApi, RecordedRequest } from '../../test/fakeApi';
import { createFakeApi } from '../../test/fakeApi';
import type { Json } from '../../test/fixtures';
import {
  accountJson,
  errorJson,
  faultedOrderJson,
  filledOrderJson,
  marketDataJson,
  partialOrderJson,
  PERSISTENCE_UNAVAILABLE_MESSAGE,
  rejectedOrderJson,
  riskRejectionJson,
} from '../../test/fixtures';
import {
  accountPath,
  rememberSelectedAccount,
  renderWithServices,
  serveAccount,
  serveServer,
} from '../../test/renderWithServices';

const ordersPath = `${accountPath}/orders`;

type Body = Record<string, unknown>;

function sentBody(request: RecordedRequest | undefined): Body {
  return (request?.body ?? {}) as Body;
}

// The recorded order the server would return for `request`, starting from a fixture.
function recordedFor(request: RecordedRequest, fixture: Json): Json {
  const body = sentBody(request);
  return {
    ...fixture,
    client_order_id: body.client_order_id,
    instrument: body.instrument,
    side: body.side,
    type: body.type,
    quantity: body.quantity,
    limit_price: body.limit_price ?? null,
  };
}

async function openTicket(api: FakeApi = createFakeApi()) {
  rememberSelectedAccount();
  serveAccount(api);
  const rendered = renderWithServices(<App />, { api });
  const ticket = await screen.findByRole('region', { name: 'Order ticket' });
  await within(ticket).findByText('Best bid');
  return { ...rendered, ticket };
}

async function reviewMarketBuy(user: ReturnType<typeof renderWithServices>['user'], ticket: HTMLElement, quantity = '10') {
  await user.type(within(ticket).getByLabelText('Quantity (AAPL)'), quantity);
  await user.click(within(ticket).getByRole('button', { name: 'Review order' }));
}

describe('order ticket form', () => {
  it('asks for a limit price only for limit orders', async () => {
    const { user, ticket } = await openTicket();
    expect(within(ticket).queryByLabelText('Limit price')).not.toBeInTheDocument();
    await user.click(within(ticket).getByRole('radio', { name: 'Limit' }));
    expect(within(ticket).getByLabelText('Limit price')).toBeInTheDocument();
    await user.click(within(ticket).getByRole('radio', { name: 'Market' }));
    expect(within(ticket).queryByLabelText('Limit price')).not.toBeInTheDocument();
  });

  it('validates the order before review and sends nothing', async () => {
    const { user, ticket, api } = await openTicket();
    await user.type(within(ticket).getByLabelText('Quantity (AAPL)'), '0.123456789');
    await user.click(within(ticket).getByRole('radio', { name: 'Limit' }));
    await user.click(within(ticket).getByRole('button', { name: 'Review order' }));

    expect(within(ticket).getByLabelText('Quantity (AAPL)')).toHaveAccessibleDescription(
      expect.stringContaining('Use at most 8 decimal places.'),
    );
    expect(within(ticket).getByLabelText('Limit price')).toHaveAccessibleDescription(
      expect.stringContaining('Required.'),
    );
    expect(within(ticket).queryByRole('button', { name: 'Submit paper order' })).not.toBeInTheDocument();
    expect(api.requestsTo('POST', ordersPath)).toHaveLength(0);
  });

  it('reviews exactly what will be sent before submitting', async () => {
    const { user, ticket, api } = await openTicket();
    await user.click(within(ticket).getByRole('radio', { name: 'Sell' }));
    await user.click(within(ticket).getByRole('radio', { name: 'Limit' }));
    await user.type(within(ticket).getByLabelText('Quantity (AAPL)'), '1.50');
    await user.type(within(ticket).getByLabelText('Limit price'), '300.0');
    await user.click(within(ticket).getByRole('button', { name: 'Review order' }));

    const review = within(ticket).getByRole('region', { name: 'Review paper order' });
    expect(within(review).getByText('1.50')).toBeInTheDocument();
    expect(within(review).getByText('300.0')).toBeInTheDocument();
    expect(within(review).getByText(/^web-[0-9a-z]+-[0-9a-z]{10}$/)).toBeInTheDocument();
    expect(api.requestsTo('POST', ordersPath)).toHaveLength(0);
  });

  it('can be completed with the keyboard alone', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    api.on('POST', ordersPath, (request) => ({
      status: 201,
      body: { order: recordedFor(request, filledOrderJson()), account: accountJson() },
    }));

    within(ticket).getByRole('radio', { name: 'Buy' }).focus();
    await user.keyboard('{ArrowRight}');
    expect(within(ticket).getByRole('radio', { name: 'Sell' })).toBeChecked();
    within(ticket).getByLabelText('Quantity (AAPL)').focus();
    await user.keyboard('1.5{Enter}');

    const submit = within(ticket).getByRole('button', { name: 'Submit paper order' });
    expect(submit).toHaveFocus();
    await user.keyboard('{Enter}');

    expect(await within(ticket).findByText('Filled 1.5 at 335.82')).toBeInTheDocument();
    expect(sentBody(api.requestsTo('POST', ordersPath)[0])).toMatchObject({ side: 'sell', type: 'market', quantity: '1.5' });
  });
});

describe('recorded outcomes', () => {
  it('shows a fill and refreshes the account views', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    api.on('POST', ordersPath, (request) => ({
      status: 201,
      body: { order: recordedFor(request, filledOrderJson()), account: accountJson() },
    }));
    const portfolioReads = api.requestsTo('GET', `${accountPath}/portfolio`).length;
    await reviewMarketBuy(user, ticket, '1.5');
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));

    expect(await within(ticket).findByText('Filled 1.5 at 335.82')).toBeInTheDocument();
    expect(within(ticket).getByText('503.73')).toBeInTheDocument();
    expect(within(ticket).getByText('Recorded in the paper journal.', { exact: false })).toBeInTheDocument();
    const [sent] = api.requestsTo('POST', ordersPath);
    expect(sent?.body).toEqual({
      client_order_id: sentBody(sent).client_order_id,
      source: { kind: 'manual', id: 'web-ticket' },
      instrument: 'AAPL',
      side: 'buy',
      type: 'market',
      quantity: '1.5',
      time_in_force: 'ioc',
    });
    expect(within(ticket).getByText(String(sentBody(sent).client_order_id))).toBeInTheDocument();
    await waitFor(() => {
      expect(api.requestsTo('GET', `${accountPath}/portfolio`).length).toBeGreaterThan(portfolioReads);
    });
  });

  it('shows a partial fill and the cancelled remainder', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    api.on('POST', ordersPath, (request) => ({
      status: 201,
      body: { order: recordedFor(request, partialOrderJson()), account: accountJson() },
    }));
    await reviewMarketBuy(user, ticket);
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));

    expect(await within(ticket).findByText('Partially filled 4 of 10 at 337; 6 cancelled')).toBeInTheDocument();
    expect(within(ticket).getByText('Displayed liquidity was exhausted.')).toBeInTheDocument();
  });

  it('presents a recorded risk rejection as a rejection, not a success', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    api.on('POST', ordersPath, (request) => ({
      status: 422,
      body: {
        error: riskRejectionJson(),
        order: recordedFor(request, rejectedOrderJson()),
        account: accountJson(),
      },
    }));
    await reviewMarketBuy(user, ticket, '900');
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));

    expect(await within(ticket).findByText('Rejected by pre-trade risk')).toBeInTheDocument();
    expect(within(ticket).getByText('Not enough cash for the order and fee', { exact: false })).toBeInTheDocument();
    expect(within(ticket).getByText('insufficient_cash')).toBeInTheDocument();
    expect(within(ticket).queryByText(/^Filled/)).not.toBeInTheDocument();
    expect(within(ticket).getByText(/consumes its client order ID/)).toBeInTheDocument();
  });

  it('presents a recorded processing fault', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    api.on('POST', ordersPath, (request) => {
      const order = recordedFor(request, faultedOrderJson());
      return { status: 500, body: { error: order.rejection, order, account: accountJson({ status: 'halted' }) } };
    });
    await reviewMarketBuy(user, ticket);
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));
    expect(await within(ticket).findByText('Processing fault: the account is halted')).toBeInTheDocument();
    expect(within(ticket).getByText('accounting_invariant_violation')).toBeInTheDocument();
  });
});

describe('unrecorded and uncertain submissions', () => {
  it('lets an order the server refused be sent again with the same ID', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    let attempt = 0;
    api.on('POST', ordersPath, (request) => {
      attempt += 1;
      return attempt === 1
        ? {
            status: 503,
            body: errorJson('market_data_unavailable', 'no top of book has been received for the instrument', {
              reason: 'no_top_of_book',
            }),
          }
        : { status: 201, body: { order: recordedFor(request, partialOrderJson()), account: accountJson() } };
    });
    await reviewMarketBuy(user, ticket);
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));

    expect(await within(ticket).findByText('Not recorded: Market data unavailable')).toBeInTheDocument();
    expect(within(ticket).getByText('no_top_of_book')).toBeInTheDocument();
    await user.click(within(ticket).getByRole('button', { name: 'Send again with the same ID' }));

    expect(await within(ticket).findByText(/^Partially filled 4 of 10/)).toBeInTheDocument();
    const [first, second] = api.requestsTo('POST', ordersPath);
    expect(second?.body).toEqual(first?.body);
  });

  it('reconciles an uncertain response by looking up the order instead of resubmitting', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    api.on('POST', ordersPath, (request) => {
      // The commit succeeded, but the response reported an outage.
      api.on('GET', `${ordersPath}/${String(sentBody(request).client_order_id)}`, {
        status: 200,
        body: { order: recordedFor(request, partialOrderJson()) },
      });
      return { status: 503, body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE) };
    });
    await reviewMarketBuy(user, ticket);
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));

    expect(await within(ticket).findByText(/^Partially filled 4 of 10/)).toBeInTheDocument();
    expect(within(ticket).getByText(/confirmed by looking up the client order ID/)).toBeInTheDocument();
    expect(api.requestsTo('POST', ordersPath)).toHaveLength(1);
  });

  it('resubmits with the identical ID only after a lookup proves nothing was recorded', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    let attempt = 0;
    api.on('POST', ordersPath, (request) => {
      attempt += 1;
      api.on('GET', `${ordersPath}/${String(sentBody(request).client_order_id)}`, {
        status: 404,
        body: errorJson('order_not_found', 'paper order does not exist', { field: 'client_order_id' }),
      });
      return attempt === 1
        ? { status: 500, body: errorJson('internal_error', 'internal server error') }
        : { status: 201, body: { order: recordedFor(request, partialOrderJson()), account: accountJson() } };
    });
    await reviewMarketBuy(user, ticket);
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));

    expect(await within(ticket).findByText('Confirmed not recorded')).toBeInTheDocument();
    expect(within(ticket).queryByRole('button', { name: 'Review order' })).not.toBeInTheDocument();
    await user.click(within(ticket).getByRole('button', { name: 'Send again with the same ID' }));

    expect(await within(ticket).findByText(/^Partially filled 4 of 10/)).toBeInTheDocument();
    const [first, second] = api.requestsTo('POST', ordersPath);
    expect(second?.body).toEqual(first?.body);
  });

  it('treats a network failure as uncertain and keeps checking until the lookup answers', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    let lookups = 0;
    api.on('POST', ordersPath, (request) => {
      const id = String(sentBody(request).client_order_id);
      api.on('GET', `${ordersPath}/${id}`, () => {
        lookups += 1;
        return lookups === 1
          ? { status: 503, body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE) }
          : { status: 200, body: { order: recordedFor(request, partialOrderJson()) } };
      });
      throw new TypeError('Failed to fetch');
    });
    await reviewMarketBuy(user, ticket);
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));

    expect(await within(ticket).findByText('Outcome still unknown')).toBeInTheDocument();
    expect(within(ticket).queryByRole('button', { name: /Send again/ })).not.toBeInTheDocument();
    await user.click(within(ticket).getByRole('button', { name: 'Check again' }));

    expect(await within(ticket).findByText(/^Partially filled 4 of 10/)).toBeInTheDocument();
    expect(api.requestsTo('POST', ordersPath)).toHaveLength(1);
  });

  it('follows a duplicate-ID answer to a retry back to the recorded original', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    let attempt = 0;
    api.on('POST', ordersPath, (request) => {
      attempt += 1;
      const lookupPath = `${ordersPath}/${String(sentBody(request).client_order_id)}`;
      if (attempt === 1) {
        api.on('GET', lookupPath, {
          status: 404,
          body: errorJson('order_not_found', 'paper order does not exist', { field: 'client_order_id' }),
        });
        return { status: 503, body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE) };
      }
      // The original commit completed after the first lookup.
      api.on('GET', lookupPath, { status: 200, body: { order: recordedFor(request, partialOrderJson()) } });
      return {
        status: 409,
        body: errorJson('duplicate_client_order_id', 'client_order_id was already used for this account', {
          field: 'client_order_id',
        }),
      };
    });
    await reviewMarketBuy(user, ticket);
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));
    await user.click(await within(ticket).findByRole('button', { name: 'Send again with the same ID' }));

    expect(await within(ticket).findByText(/^Partially filled 4 of 10/)).toBeInTheDocument();
    expect(api.requestsTo('POST', ordersPath)).toHaveLength(2);
  });

  it('never presents a different order that happens to share the ID', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    api.on('POST', ordersPath, (request) => {
      api.on('GET', `${ordersPath}/${String(sentBody(request).client_order_id)}`, {
        status: 200,
        body: { order: { ...recordedFor(request, partialOrderJson()), quantity: '11', unfilled_quantity: '7' } },
      });
      return { status: 503, body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE) };
    });
    await reviewMarketBuy(user, ticket);
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));
    expect(await within(ticket).findByText('This client order ID belongs to a different order')).toBeInTheDocument();
    expect(within(ticket).queryByText(/^Partially filled 4 of 10/)).not.toBeInTheDocument();
  });

  it('sends one request however often submit is pressed', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    let release: () => void = () => undefined;
    api.on(
      'POST',
      ordersPath,
      (request) =>
        new Promise((resolve) => {
          release = () => {
            resolve({ status: 201, body: { order: recordedFor(request, partialOrderJson()), account: accountJson() } });
          };
        }),
    );
    await reviewMarketBuy(user, ticket);
    const submit = within(ticket).getByRole('button', { name: 'Submit paper order' });
    await user.dblClick(submit);
    expect(await within(ticket).findByText('Submitting paper order…')).toBeInTheDocument();
    expect(within(ticket).queryByRole('button', { name: 'Submit paper order' })).not.toBeInTheDocument();
    release();

    expect(await within(ticket).findByText(/^Partially filled 4 of 10/)).toBeInTheDocument();
    expect(api.requestsTo('POST', ordersPath)).toHaveLength(1);
  });

  it('generates a new client order ID for each new order', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    api.on('POST', ordersPath, (request) => ({
      status: 201,
      body: { order: recordedFor(request, partialOrderJson()), account: accountJson() },
    }));
    await reviewMarketBuy(user, ticket);
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));
    await user.click(await within(ticket).findByRole('button', { name: 'New order' }));
    await user.click(within(ticket).getByRole('button', { name: 'Review order' }));
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));
    await within(ticket).findByRole('button', { name: 'New order' });

    const [first, second] = api.requestsTo('POST', ordersPath).map((request) => sentBody(request).client_order_id);
    expect(first).not.toEqual(second);
  });
});

describe('symbols', () => {
  it('does not offer review while the selected symbol is known to have no executable quote', async () => {
    const api = createFakeApi();
    rememberSelectedAccount();
    serveAccount(api);
    serveServer(api, marketDataJson({ reasons: { AAPL: 'no_top_of_book' } }));
    renderWithServices(<App />, { api });

    const ticket = await screen.findByRole('region', { name: 'Order ticket' });
    expect(await within(ticket).findByText('No usable AAPL quote right now')).toBeInTheDocument();
    await within(ticket).findByRole('button', { name: 'Review order' });
    expect(within(ticket).getByRole('button', { name: 'Review order' })).toBeDisabled();
    expect(api.requestsTo('POST', ordersPath)).toHaveLength(0);
  });

  it('trades the symbol chosen in the ticket and keeps a reviewed order on its own symbol', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    api.on('POST', ordersPath, (request) => ({
      status: 201,
      body: { order: recordedFor(request, filledOrderJson()), account: accountJson() },
    }));
    await user.selectOptions(within(ticket).getByRole('combobox', { name: 'Symbol' }), 'SPY');
    expect(within(ticket).getByRole('group', { name: 'Reference quote for SPY' })).toHaveTextContent('769.78');
    expect(within(ticket).getByRole('group', { name: 'Position and account limits' })).toHaveTextContent('Held (SPY)None');
    await user.type(within(ticket).getByLabelText('Quantity (SPY)'), '1.5');
    await user.click(within(ticket).getByRole('button', { name: 'Review order' }));

    // Choosing another symbol elsewhere does not change the order under review.
    const watchlist = screen.getByRole('region', { name: 'Watchlist' });
    await user.click(within(watchlist).getByRole('radio', { name: 'AAPL' }));
    expect(within(ticket).getByRole('group', { name: 'Reference quote for SPY' })).toBeInTheDocument();
    expect(within(ticket).getByText('Symbol', { selector: 'dt' }).nextElementSibling).toHaveTextContent('SPY');
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));

    expect(await within(ticket).findByText(/^Filled 1.5/)).toBeInTheDocument();
    expect(sentBody(api.requestsTo('POST', ordersPath)[0]).instrument).toBe('SPY');
    // A new order follows the selection again.
    await user.click(within(ticket).getByRole('button', { name: 'New order' }));
    expect(within(ticket).getByRole('combobox', { name: 'Symbol' })).toHaveValue('AAPL');
    expect(within(ticket).getByRole('group', { name: 'Position and account limits' })).toHaveTextContent('Held (AAPL)2.5');
  });

  it('never reconciles an uncertain order to a recorded order for another symbol', async () => {
    const api = createFakeApi();
    const { user, ticket } = await openTicket(api);
    api.on('POST', ordersPath, (request) => {
      api.on('GET', `${ordersPath}/${String(sentBody(request).client_order_id)}`, {
        status: 200,
        body: { order: { ...recordedFor(request, partialOrderJson()), instrument: 'SPY' } },
      });
      return { status: 503, body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE) };
    });
    await reviewMarketBuy(user, ticket);
    await user.click(within(ticket).getByRole('button', { name: 'Submit paper order' }));
    expect(await within(ticket).findByText('This client order ID belongs to a different order')).toBeInTheDocument();
    expect(api.requestsTo('POST', ordersPath)).toHaveLength(1);
  });
});

describe('halted accounts', () => {
  it('explains the halt and does not offer a review', async () => {
    rememberSelectedAccount();
    const api = createFakeApi();
    serveAccount(api, { account: accountJson({ status: 'halted' }) });
    renderWithServices(<App />, { api });
    const ticket = await screen.findByRole('region', { name: 'Order ticket' });
    expect(await within(ticket).findByText('This account is halted')).toBeInTheDocument();
    expect(within(ticket).getByRole('button', { name: 'Review order' })).toBeDisabled();
  });
});
