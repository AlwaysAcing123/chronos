import { screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { App } from '../../App';
import { createFakeApi } from '../../test/fakeApi';
import {
  accountJson,
  errorJson,
  flatAccountJson,
  healthJson,
  marketJson,
  PERSISTENCE_UNAVAILABLE_MESSAGE,
  portfolioJson,
} from '../../test/fixtures';
import { accountPath, rememberSelectedAccount, renderWithServices, serveAccount } from '../../test/renderWithServices';

async function openPortfolio(api = createFakeApi()) {
  rememberSelectedAccount();
  const rendered = renderWithServices(<App />, { api });
  const portfolio = await screen.findByRole('region', { name: 'Portfolio' });
  return { ...rendered, portfolio };
}

function factValue(container: HTMLElement, label: string): string | null {
  const term = within(container).getByText(label, { selector: 'dt' });
  return term.nextElementSibling?.textContent ?? null;
}

describe('portfolio', () => {
  it('renders every value as the exact server string', async () => {
    const api = createFakeApi();
    serveAccount(api);
    const { portfolio } = await openPortfolio(api);
    await within(portfolio).findByText('99994.58');

    expect(factValue(portfolio, 'Equity')).toBe('99994.58');
    expect(factValue(portfolio, 'Total PnL')).toBe('-5.42');
    expect(factValue(portfolio, 'Return')).toBe('-0.00542 %');
    expect(factValue(portfolio, 'Unrealized PnL')).toBe('-2.95');
    expect(factValue(portfolio, 'Position market value')).toBe('839.55');
    expect(factValue(portfolio, 'Mark price (best bid)')).toBe('335.82');
    expect(factValue(portfolio, 'Realized PnL')).toBe('-1.77');
    expect(factValue(portfolio, 'Fees paid')).toBe('0.7');
    expect(factValue(portfolio, 'Account version')).toBe('2');
    expect(within(portfolio).getByText(/not guaranteed liquidation value/)).toBeInTheDocument();
  });

  it('keeps precision a JavaScript number would lose', async () => {
    const api = createFakeApi();
    serveAccount(api, {
      portfolio: portfolioJson({ equity: '92233720368.54775807', cash: '92233720368.54775807' }),
    });
    const { portfolio } = await openPortfolio(api);
    expect(await within(portfolio).findAllByText('92233720368.54775807')).toHaveLength(2);
  });

  it('shows unusable market data as unavailable, never as zero', async () => {
    const api = createFakeApi();
    serveAccount(api, { account: flatAccountJson() });
    api.on('GET', `${accountPath}/portfolio`, {
      status: 503,
      body: errorJson('market_data_unavailable', 'the quote is older than the maximum quote age', {
        reason: 'stale_market_state',
      }),
    });
    const { portfolio } = await openPortfolio(api);

    expect(await within(portfolio).findByText('Market data unavailable')).toBeInTheDocument();
    expect(within(portfolio).getByText('stale_market_state')).toBeInTheDocument();
    expect(factValue(portfolio, 'Equity')).toBe('Unavailable');
    expect(factValue(portfolio, 'Total PnL')).toBe('Unavailable');
    expect(factValue(portfolio, 'Return')).toBe('Unavailable');
    // The durable ledger still comes from the account resource.
    await waitFor(() => {
      expect(factValue(portfolio, 'Cash')).toBe('100000');
    });
    expect(within(portfolio).queryByText('Position market value')).not.toBeInTheDocument();

    const market = screen.getByRole('region', { name: 'Market · AAPL' });
    expect(within(market).getByText('No usable quote for this account')).toBeInTheDocument();
  });

  it('reports a failed valuation without values', async () => {
    const api = createFakeApi();
    serveAccount(api);
    api.on('GET', `${accountPath}/portfolio`, {
      status: 500,
      body: errorJson('valuation_failed', 'portfolio values do not reconcile', { reason: 'accounting_identity_failed' }),
    });
    const { portfolio } = await openPortfolio(api);
    expect(await within(portfolio).findByText('The portfolio could not be valued exactly')).toBeInTheDocument();
    expect(factValue(portfolio, 'Equity')).toBe('Unavailable');
  });

  it('shows a persistence outage with a retry instead of stale values', async () => {
    const api = createFakeApi();
    serveAccount(api);
    let outage = true;
    api.on('GET', `${accountPath}/portfolio`, () =>
      outage
        ? { status: 503, body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE) }
        : { status: 200, body: { portfolio: portfolioJson() } },
    );
    const { portfolio, user } = await openPortfolio(api);

    expect(await within(portfolio).findByText('The paper-trading database is unavailable')).toBeInTheDocument();
    expect(within(portfolio).queryByText('99994.58')).not.toBeInTheDocument();
    outage = false;
    await user.click(within(portfolio).getByRole('button', { name: 'Retry' }));
    expect(await within(portfolio).findByText('99994.58')).toBeInTheDocument();
  });

  it('shows the loading state before the first valuation', async () => {
    const api = createFakeApi();
    serveAccount(api);
    api.on('GET', `${accountPath}/portfolio`, () => new Promise(() => undefined));
    const { portfolio } = await openPortfolio(api);
    expect(within(portfolio).getByText('Loading valuation…')).toBeInTheDocument();
  });
});

describe('market state', () => {
  it('shows the quote, its lineage, and its quality flags', async () => {
    const api = createFakeApi();
    serveAccount(api, {
      portfolio: portfolioJson({ market: marketJson({ quality_flags: ['replay', 'snapshot'] }) }),
    });
    rememberSelectedAccount();
    renderWithServices(<App />, { api });
    const market = await screen.findByRole('region', { name: 'Market · AAPL' });

    await within(market).findByText('121297');
    expect(factValue(market, 'Best bid × size')).toBe('335.82 × 80');
    expect(factValue(market, 'Best ask × size')).toBe('337 × 4');
    expect(factValue(market, 'Event time (UTC)')).toBe('2026-09-25 13:39:59.960824207');
    expect(factValue(market, 'Receive time (UTC)')).toBe('2026-09-25 13:39:59.960922473');
    expect(within(market).getByText('Replay')).toBeInTheDocument();
    expect(within(market).getByText('Snapshot')).toBeInTheDocument();
    expect(await within(market).findByText('databento-equs-mini-mbp1-20260925-1330-1340')).toBeInTheDocument();
  });

  it('makes an interrupted stream unmistakable', async () => {
    const api = createFakeApi();
    serveAccount(api);
    api.on('GET', '/health', {
      status: 200,
      body: healthJson({
        market_data: {
          stream_id: 'stream-a',
          state: 'interrupted',
          last_ingest_sequence: '41',
          market_time: '2026-09-25T13:39:59.969574414Z',
          interruption: 'market data stream skipped an ingest sequence',
        },
      }),
    });
    api.on('GET', `${accountPath}/portfolio`, {
      status: 503,
      body: errorJson('market_data_unavailable', 'the market-data stream is interrupted', {
        reason: 'stream_interrupted',
      }),
    });
    rememberSelectedAccount();
    renderWithServices(<App />, { api });

    expect(await screen.findByText('The market-data stream is interrupted')).toBeInTheDocument();
    expect(screen.getByText('Market stream interrupted')).toBeInTheDocument();
    const ticket = screen.getByRole('region', { name: 'Order ticket' });
    expect(await within(ticket).findByText('No usable quote right now')).toBeInTheDocument();
  });

  it('reports an unreachable API in the header', async () => {
    const api = createFakeApi();
    serveAccount(api);
    api.failNext('GET', '/health');
    rememberSelectedAccount();
    renderWithServices(<App />, { api });
    expect(await within(screen.getByRole('banner')).findByText('API unreachable')).toBeInTheDocument();
  });
});

describe('polling', () => {
  it('re-reads account data on the configured interval and stops for a missing account', async () => {
    const api = createFakeApi();
    serveAccount(api, { account: accountJson() });
    rememberSelectedAccount();
    renderWithServices(<App />, { api, refresh: { healthMs: false, accountMs: 25 } });
    await screen.findByText('99994.58');

    await waitFor(() => {
      expect(api.requestsTo('GET', `${accountPath}/portfolio`).length).toBeGreaterThanOrEqual(3);
    });
    expect(api.requestsTo('GET', '/health')).toHaveLength(1);

    const missing = { status: 404, body: errorJson('account_not_found', 'paper account does not exist') };
    api.on('GET', accountPath, missing);
    api.on('GET', `${accountPath}/portfolio`, missing);
    await screen.findByText('Paper account not found: demo-1');
    const reads = api.requestsTo('GET', `${accountPath}/portfolio`).length;
    await new Promise((resolve) => setTimeout(resolve, 120));
    expect(api.requestsTo('GET', `${accountPath}/portfolio`)).toHaveLength(reads);
  });

  it('labels the view as polled and shows the last successful refresh', async () => {
    const api = createFakeApi();
    serveAccount(api);
    rememberSelectedAccount();
    renderWithServices(<App />, { api, refresh: { healthMs: 5_000, accountMs: 2_000 } });
    const header = screen.getByRole('banner');
    expect(await within(header).findByText(/^Polling every 2 s · last refresh \d/)).toBeInTheDocument();
  });
});
