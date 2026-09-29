import { screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { App } from './App';
import { RECENT_ACCOUNTS_KEY, SELECTED_ACCOUNT_KEY } from './state/accountSelection';
import { createFakeApi } from './test/fakeApi';
import { ACCOUNT_ID, errorJson, flatAccountJson, healthJson, portfolioJson } from './test/fixtures';
import { accountPath, rememberSelectedAccount, renderWithServices, serveAccount } from './test/renderWithServices';

describe('account entry', () => {
  it('always states that trading is simulated', async () => {
    const api = createFakeApi();
    api.on('GET', '/health', { status: 200, body: healthJson() });
    renderWithServices(<App />, { api });
    const notice = screen.getByRole('note', { name: 'Paper trading notice' });
    expect(notice).toHaveTextContent('PAPER TRADING');
    expect(notice).toHaveTextContent('No order ever reaches a broker or exchange, and no real money is involved.');
    expect(await screen.findByText('API online')).toBeInTheDocument();
  });

  it('creates an account with a server-listed instrument and opens it', async () => {
    const api = createFakeApi();
    serveAccount(api, { account: flatAccountJson(), portfolio: portfolioJson(), orders: [], fills: [] });
    // Configuration order, not alphabetical.
    api.on('GET', '/health', { status: 200, body: healthJson({ tradeable_instruments: ['SPY', 'AAPL'] }) });
    api.on('POST', '/api/v1/accounts', {
      status: 201,
      body: { account: flatAccountJson({ instrument: 'AAPL', starting_cash: '2500.75', cash: '2500.75' }) },
    });
    const { user } = renderWithServices(<App />, { api });

    await screen.findByRole('option', { name: 'AAPL' });
    const instrument = screen.getByRole('combobox', { name: 'Instrument' });
    expect(within(instrument).getAllByRole('option').map((option) => option.textContent)).toEqual(['SPY', 'AAPL']);
    await user.selectOptions(instrument, 'AAPL');
    const create = screen.getByRole('region', { name: 'Create a paper account' });
    await user.type(within(create).getByLabelText('Account ID'), ACCOUNT_ID);
    const cash = within(create).getByLabelText('Starting cash (simulated)');
    await user.clear(cash);
    await user.type(cash, '2500.750');
    await user.click(screen.getByRole('button', { name: 'Create paper account' }));

    expect(await screen.findByRole('heading', { name: 'Order ticket' })).toBeInTheDocument();
    expect(api.requestsTo('POST', '/api/v1/accounts')[0]?.body).toEqual({
      account_id: ACCOUNT_ID,
      instrument: 'AAPL',
      // Exactly as typed: the browser does not normalise decimals.
      starting_cash: '2500.750',
    });
    expect(window.localStorage.getItem(SELECTED_ACCOUNT_KEY)).toBe(ACCOUNT_ID);
  });

  it('validates account creation before sending anything', async () => {
    const api = createFakeApi();
    api.on('GET', '/health', { status: 200, body: healthJson() });
    const { user } = renderWithServices(<App />, { api });
    await screen.findByRole('option', { name: 'AAPL' });
    const create = screen.getByRole('region', { name: 'Create a paper account' });
    await user.type(within(create).getByLabelText('Account ID'), '-bad id');
    const cash = within(create).getByLabelText('Starting cash (simulated)');
    await user.clear(cash);
    await user.type(cash, '1e6');
    await user.click(screen.getByRole('button', { name: 'Create paper account' }));

    expect(within(create).getByLabelText('Account ID')).toHaveAttribute('aria-invalid', 'true');
    expect(cash).toHaveAccessibleDescription(/without signs, spaces, separators, or exponents/);
    expect(api.requestsTo('POST', '/api/v1/accounts')).toHaveLength(0);
  });

  it('offers to open an account whose ID is already taken', async () => {
    const api = createFakeApi();
    api.on('GET', '/health', { status: 200, body: healthJson() });
    api.on('POST', '/api/v1/accounts', {
      status: 409,
      body: errorJson('account_already_exists', 'paper account already exists', { field: 'account_id' }),
    });
    const { user } = renderWithServices(<App />, { api });
    await screen.findByRole('option', { name: 'AAPL' });
    const create = screen.getByRole('region', { name: 'Create a paper account' });
    await user.type(within(create).getByLabelText('Account ID'), ACCOUNT_ID);
    await user.click(screen.getByRole('button', { name: 'Create paper account' }));

    expect(await screen.findByText('Account ID already in use')).toBeInTheDocument();
    serveAccount(api);
    await user.click(screen.getByRole('button', { name: `Open ${ACCOUNT_ID}` }));
    expect(await screen.findByRole('heading', { name: 'Order ticket' })).toBeInTheDocument();
  });

  it('opens an existing account only after the server confirms it', async () => {
    const api = createFakeApi();
    api.on('GET', '/health', { status: 200, body: healthJson() });
    api.on('GET', '/api/v1/accounts/nobody', {
      status: 404,
      body: errorJson('account_not_found', 'paper account does not exist', { field: 'account_id' }),
    });
    const { user } = renderWithServices(<App />, { api });
    const open = screen.getByRole('region', { name: 'Open an existing account' });
    await user.type(within(open).getByLabelText('Account ID'), 'nobody');
    await user.click(within(open).getByRole('button', { name: 'Open account' }));

    expect(await within(open).findByText('No paper account with this ID exists on this server.')).toBeInTheDocument();
    expect(window.localStorage.getItem(SELECTED_ACCOUNT_KEY)).toBeNull();

    serveAccount(api);
    await user.clear(within(open).getByLabelText('Account ID'));
    await user.type(within(open).getByLabelText('Account ID'), ACCOUNT_ID);
    await user.click(within(open).getByRole('button', { name: 'Open account' }));
    expect(await screen.findByRole('heading', { name: 'Order ticket' })).toBeInTheDocument();
  });
});

describe('selected account persistence', () => {
  it('restores the selected account after a reload', async () => {
    rememberSelectedAccount();
    const api = createFakeApi();
    serveAccount(api);
    renderWithServices(<App />, { api });

    expect(await screen.findByRole('heading', { name: 'Order ticket' })).toBeInTheDocument();
    const header = screen.getByRole('banner');
    expect(within(header).getByText(ACCOUNT_ID)).toBeInTheDocument();
    expect(await within(header).findByText('AAPL')).toBeInTheDocument();
  });

  it('stores only account IDs', async () => {
    rememberSelectedAccount();
    const api = createFakeApi();
    serveAccount(api);
    renderWithServices(<App />, { api });
    await screen.findByText('99994.58');
    const stored = Object.keys(window.localStorage).map((key) => [key, window.localStorage.getItem(key)]);
    expect(stored).toEqual(
      expect.arrayContaining([
        [SELECTED_ACCOUNT_KEY, ACCOUNT_ID],
        [RECENT_ACCOUNTS_KEY, JSON.stringify([ACCOUNT_ID])],
      ]),
    );
    expect(stored).toHaveLength(2);
  });

  it('explains a remembered account that no longer exists and can forget it', async () => {
    rememberSelectedAccount('gone-1');
    const api = createFakeApi();
    api.on('GET', '/health', { status: 200, body: healthJson() });
    const missing = { status: 404, body: errorJson('account_not_found', 'paper account does not exist') };
    api.on('GET', '/api/v1/accounts/gone-1', missing);
    api.on('GET', '/api/v1/accounts/gone-1/portfolio', missing);
    const { user } = renderWithServices(<App />, { api });

    expect(await screen.findByText('Paper account not found: gone-1')).toBeInTheDocument();
    expect(screen.queryByRole('heading', { name: 'Order ticket' })).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Remove from recent accounts' }));

    expect(await screen.findByRole('heading', { name: 'Open a paper-trading account' })).toBeInTheDocument();
    expect(window.localStorage.getItem(RECENT_ACCOUNTS_KEY)).toBe('[]');
    expect(window.localStorage.getItem(SELECTED_ACCOUNT_KEY)).toBeNull();
  });

  it('switches back to account selection, keeping the recent list', async () => {
    rememberSelectedAccount();
    const api = createFakeApi();
    serveAccount(api);
    const { user } = renderWithServices(<App />, { api });
    await user.click(await screen.findByRole('button', { name: 'Switch account' }));

    const recent = await screen.findByRole('navigation', { name: 'Recently opened in this browser' });
    expect(within(recent).getByRole('button', { name: `Open ${ACCOUNT_ID}` })).toBeInTheDocument();
    await waitFor(() => {
      expect(window.localStorage.getItem(SELECTED_ACCOUNT_KEY)).toBeNull();
    });
  });

  it('refuses a response that claims not to be paper trading', async () => {
    rememberSelectedAccount();
    const api = createFakeApi();
    serveAccount(api);
    api.on('GET', accountPath, { status: 200, body: { account: flatAccountJson({ trading_mode: 'live' }) } });
    renderWithServices(<App />, { api });
    expect(await screen.findByText('Unexpected response')).toBeInTheDocument();
    expect(screen.queryByRole('heading', { name: 'Order ticket' })).not.toBeInTheDocument();
  });
});
