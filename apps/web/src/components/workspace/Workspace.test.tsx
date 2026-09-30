import { screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { App } from '../../App';
import { createFakeApi } from '../../test/fakeApi';
import { SELECTED_INSTRUMENT_KEY } from '../../state/instrumentSelection';
import {
  accountJson,
  closedPositionJson,
  errorJson,
  flatAccountJson,
  marketDataJson,
  marketJson,
  marketStreamJson,
  PERSISTENCE_UNAVAILABLE_MESSAGE,
  portfolioJson,
  replaySourceJson,
  spyMarketJson,
  unmarkableJson,
  valuedPositionJson,
} from '../../test/fixtures';
import {
  accountPath,
  rememberSelectedAccount,
  renderWithServices,
  serveAccount,
  serveServer,
} from '../../test/renderWithServices';

async function openPortfolio(api = createFakeApi()) {
  rememberSelectedAccount();
  const rendered = renderWithServices(<App />, { api });
  const portfolio = await screen.findByRole('region', { name: 'Portfolio' });
  return { ...rendered, portfolio };
}

// The text of each cell in the row of `table` whose first cell starts with `key`.
function rowCells(container: HTMLElement, table: string, key: string): (string | null)[] {
  const region = within(container).getByRole('region', { name: table });
  const row = within(region)
    .getAllByRole('row')
    .find((candidate) => within(candidate).queryAllByRole('cell')[0]?.textContent.startsWith(key));
  if (row === undefined) {
    throw new Error(`no ${key} row in ${table}`);
  }
  return within(row).getAllByRole('cell').map((cell) => cell.textContent);
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
    expect(factValue(portfolio, 'Market value')).toBe('839.55');
    expect(factValue(portfolio, 'Realized PnL')).toBe('-1.77');
    expect(factValue(portfolio, 'Fees paid')).toBe('0.7');
    expect(factValue(portfolio, 'Account version')).toBe('2');
    expect(within(portfolio).getByText(/not guaranteed liquidation value/)).toBeInTheDocument();
    expect(rowCells(portfolio, 'Open positions', 'AAPL')).toEqual([
      'AAPL', '2.5', '0', '2.5', '337', '842.5', '335.82', '839.55', '-2.95', '-1.77', '0.7',
    ]);
  });

  it('lists every position and closed ledger with the server totals', async () => {
    const api = createFakeApi();
    serveAccount(api, {
      portfolio: portfolioJson({
        positions: [
          valuedPositionJson(),
          valuedPositionJson({
            instrument: 'SPY',
            quantity: '2',
            open_cost_basis: '1539.56',
            average_entry_price: '769.78',
            gross_realized_pnl: '0',
            fees_paid: '0.35',
            mark_price: '769.7',
            market: spyMarketJson(),
            market_value: '1539.4',
            unrealized_pnl: '-0.16',
          }),
        ],
        closed_positions: [closedPositionJson({ instrument: 'MSFT' })],
      }),
    });
    const { portfolio } = await openPortfolio(api);
    await within(portfolio).findByText('1539.4');
    const open = within(portfolio).getByRole('region', { name: 'Open positions' });
    expect(within(open).getAllByRole('row').slice(1).map((row) => within(row).getAllByRole('cell')[0]?.textContent))
      .toEqual(['AAPL', 'SPY']);
    expect(rowCells(portfolio, 'Open positions', 'SPY')).toEqual([
      'SPY', '2', '0', '2', '769.78', '1539.56', '769.7', '1539.4', '-0.16', '0', '0.35',
    ]);
    expect(rowCells(portfolio, 'Closed positions', 'MSFT')).toEqual([
      'MSFT', '2.5', '0.7', '2026-09-2513:39:59.969574414',
    ]);
  });

  it('keeps precision a JavaScript number would lose', async () => {
    const api = createFakeApi();
    serveAccount(api, {
      portfolio: portfolioJson({ equity: '92233720368.54775807', cash: '92233720368.54775807' }),
    });
    const { portfolio } = await openPortfolio(api);
    // Equity, cash, and the available cash that nothing reserves.
    expect(await within(portfolio).findAllByText('92233720368.54775807')).toHaveLength(3);
  });

  it('shows unmarkable positions as unavailable, never as zero', async () => {
    const api = createFakeApi();
    serveAccount(api);
    api.on('GET', `${accountPath}/portfolio`, {
      status: 503,
      body: unmarkableJson({ AAPL: 'stale_market_state' }),
    });
    const { portfolio } = await openPortfolio(api);

    expect(await within(portfolio).findByText('Market data unavailable')).toBeInTheDocument();
    const failures = within(portfolio).getByRole('list', { name: 'Positions without a usable mark' });
    expect(failures).toHaveTextContent("AAPL: The quote is older than the account's maximum quote age");
    expect(within(failures).getByText('stale_market_state')).toBeInTheDocument();
    expect(factValue(portfolio, 'Equity')).toBe('Unavailable');
    expect(factValue(portfolio, 'Total PnL')).toBe('Unavailable');
    expect(factValue(portfolio, 'Return')).toBe('Unavailable');
    // The durable ledger still comes from the account resource; what needs a
    // mark is unavailable rather than zero.
    await waitFor(() => {
      expect(factValue(portfolio, 'Cash')).toBe('99155.03');
    });
    expect(rowCells(portfolio, 'Open positions', 'AAPL')).toEqual([
      'AAPL', '2.5', '0', '2.5', '337', '842.5', 'Unavailable', 'Unavailable', 'Unavailable', '-1.77', '0.7',
    ]);
    expect(within(portfolio).queryByText('Market value', { selector: 'dt' })).not.toBeInTheDocument();
  });

  it('explains a whole-account market failure in words', async () => {
    const api = createFakeApi();
    serveAccount(api, { account: flatAccountJson() });
    api.on('GET', `${accountPath}/portfolio`, {
      status: 503,
      body: errorJson('market_data_unavailable', 'the account trades on another market-data stream', {
        reason: 'incompatible_market_timeline',
      }),
    });
    const { portfolio } = await openPortfolio(api);
    expect(await within(portfolio).findByText('incompatible_market_timeline')).toBeInTheDocument();
    expect(within(portfolio).getByText('No open positions.')).toBeInTheDocument();
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
    serveAccount(api);
    serveServer(api, marketDataJson({ quotes: { AAPL: marketJson({ quality_flags: ['replay', 'snapshot'] }) } }));
    rememberSelectedAccount();
    renderWithServices(<App />, { api });
    const market = await screen.findByRole('region', { name: 'Market' });

    await within(market).findByText('121297');
    expect(factValue(market, 'Best bid × size')).toBe('335.82 × 80');
    expect(factValue(market, 'Best ask × size')).toBe('337 × 4');
    expect(factValue(market, 'Event time (UTC)')).toBe('2026-09-25 13:39:59.960824207');
    expect(factValue(market, 'Receive time (UTC)')).toBe('2026-09-25 13:39:59.960922473');
    expect(within(market).getByText('Replay')).toBeInTheDocument();
    expect(within(market).getByText('Snapshot')).toBeInTheDocument();
    expect(await within(market).findByText('databento-equs-mini-mbp1-20260925-1330-1340')).toBeInTheDocument();
  });

  it('selects a symbol in the watchlist for the quote and the ticket, and remembers it', async () => {
    const api = createFakeApi();
    serveAccount(api);
    rememberSelectedAccount();
    const { user, unmount } = renderWithServices(<App />, { api });
    const market = await screen.findByRole('region', { name: 'Market' });
    const watchlist = await within(market).findByRole('region', { name: 'Watchlist' });
    expect(rowCells(market, 'Watchlist', 'AAPL')).toEqual(['AAPLSelected', '335.82 × 80', '337 × 4', 'Available', '2.5']);
    expect(rowCells(market, 'Watchlist', 'SPY')).toEqual(['SPY', '769.7 × 50', '769.78 × 340', 'Available', 'None']);
    expect(within(watchlist).getByRole('radio', { name: 'AAPL' })).toBeChecked();

    await user.click(within(watchlist).getByRole('radio', { name: 'SPY' }));
    expect(within(market).getByRole('heading', { name: 'Quote · SPY' })).toBeInTheDocument();
    expect(factValue(market, 'Best ask × size')).toBe('769.78 × 340');
    const ticket = screen.getByRole('region', { name: 'Order ticket' });
    expect(within(ticket).getByRole('combobox', { name: 'Symbol' })).toHaveValue('SPY');
    expect(within(ticket).getByLabelText('Quantity (SPY)')).toBeInTheDocument();
    expect(within(ticket).getByRole('group', { name: 'Reference quote for SPY' })).toHaveTextContent('769.78');
    expect(window.localStorage.getItem(SELECTED_INSTRUMENT_KEY)).toBe('SPY');

    // A reload keeps the symbol.
    unmount();
    renderWithServices(<App />, { api });
    const reloaded = await screen.findByRole('region', { name: 'Watchlist' });
    expect(await within(reloaded).findByRole('radio', { name: 'SPY' })).toBeChecked();
  });

  it('makes an interrupted stream unmistakable', async () => {
    const api = createFakeApi();
    serveAccount(api);
    serveServer(
      api,
      marketDataJson({
        source: replaySourceJson({
          state: 'failed',
          diagnostic: 'market state rejected an event: market replay rejected stream event: gap',
        }),
        stream: marketStreamJson({
          state: 'interrupted',
          interruption: 'market data stream skipped an ingest sequence',
        }),
        available: false,
      }),
    );
    api.on('GET', `${accountPath}/portfolio`, {
      status: 503,
      body: errorJson('market_data_unavailable', 'the market-data stream is interrupted', {
        reason: 'stream_interrupted',
      }),
    });
    rememberSelectedAccount();
    renderWithServices(<App />, { api });

    expect(await screen.findByText('The market-data stream is interrupted')).toBeInTheDocument();
    expect(await screen.findByText('Market-data source failed')).toBeInTheDocument();
    expect(screen.getByText(/market state rejected an event/)).toBeInTheDocument();
    const ticket = screen.getByRole('region', { name: 'Order ticket' });
    expect(await within(ticket).findByText('No usable AAPL quote right now')).toBeInTheDocument();
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
    renderWithServices(<App />, { api, refresh: { healthMs: false, accountMs: 25, marketMs: 25, streamedMarketMs: false } });
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
    renderWithServices(<App />, { api, refresh: { healthMs: 5_000, accountMs: 2_000, marketMs: 2_000, streamedMarketMs: 15_000 } });
    const header = screen.getByRole('banner');
    expect(await within(header).findByText(/^Polling every 2 s · last refresh \d/)).toBeInTheDocument();
  });
});
