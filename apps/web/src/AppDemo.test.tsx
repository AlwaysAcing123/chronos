import { screen, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';

import { App } from './App';
import { RECENT_ACCOUNTS_KEY, SELECTED_ACCOUNT_KEY } from './state/accountSelection';
import { demoGuideJson } from './test/demoFixtures';
import { createFakeApi } from './test/fakeApi';
import { createFakeResearch } from './test/fakeResearch';
import { ACCOUNT_ID, marketDataJson, syntheticSourceJson } from './test/fixtures';
import { rememberSelectedAccount, renderWithServices, serveAccount } from './test/renderWithServices';

// A demo workspace's build (VITE_CHRONOS_DEMO=1).
vi.mock('./state/demo', async (original) => ({ ...(await original<object>()), DEMO_ENABLED: true }));

describe('a demo build', () => {
  it('adds the Guide and leaves the selected paper account as it was', async () => {
    rememberSelectedAccount();
    const api = createFakeApi();
    serveAccount(api);
    api.on('GET', '/api/v1/market-data', {
      status: 200,
      body: { market_data: marketDataJson({ source: syntheticSourceJson() }) },
    });
    const research = createFakeResearch();
    research.on('GET', '/api/v1/research/demo', { status: 200, body: demoGuideJson() });
    window.location.hash = '#/demo';
    const { user } = renderWithServices(<App />, { api, research, demo: true });

    const navigation = screen.getByRole('navigation', { name: 'Primary' });
    expect(within(navigation).getByRole('link', { name: 'Guide' })).toHaveAttribute('aria-current', 'page');
    expect(
      await screen.findByText(new RegExp(`The account you had open, ${ACCOUNT_ID}, is still selected`)),
    ).toBeInTheDocument();
    // The source bar names the data synthetic on every screen.
    expect(within(screen.getByRole('region', { name: 'Market-data source' })).getByText('Synthetic replay')).toBeInTheDocument();

    // The guide's own link back, not the navigation's.
    const back = within(screen.getByRole('main')).getAllByRole('link', { name: 'Trading' }).at(-1);
    if (back === undefined) {
      throw new Error('the guide has no link back to trading');
    }
    await user.click(back);
    expect(await screen.findByRole('heading', { name: 'Order ticket' })).toBeInTheDocument();
    expect(window.localStorage.getItem(SELECTED_ACCOUNT_KEY)).toBe(ACCOUNT_ID);
    expect(window.localStorage.getItem(RECENT_ACCOUNTS_KEY)).toBe(JSON.stringify([ACCOUNT_ID]));
    // Nothing was sent but reads.
    expect([...api.requests, ...research.requests].every((request) => request.method === 'GET')).toBe(true);
    window.location.hash = '';
  });
});
