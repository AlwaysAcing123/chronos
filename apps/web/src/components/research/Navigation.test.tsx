import { act, screen } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';

import { App } from '../../App';
import { RESEARCH_API } from '../../api/research';
import { SELECTED_ACCOUNT_KEY } from '../../state/accountSelection';
import { createFakeApi } from '../../test/fakeApi';
import { createFakeResearch } from '../../test/fakeResearch';
import { ACCOUNT_ID } from '../../test/fixtures';
import { rememberSelectedAccount, renderWithServices, serveAccount } from '../../test/renderWithServices';
import { experimentPageJson, researchStatusJson } from '../../test/researchFixtures';

function navigate(hash: string) {
  act(() => {
    window.location.hash = hash;
    window.dispatchEvent(new HashChangeEvent('hashchange'));
  });
}

afterEach(() => {
  window.location.hash = '';
});

describe('navigation between trading and research', () => {
  it('keeps the open paper account while research is shown and after returning', async () => {
    rememberSelectedAccount();
    const api = createFakeApi();
    serveAccount(api);
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/status`, { status: 200, body: researchStatusJson() });
    research.on('GET', `${RESEARCH_API}/experiments`, { status: 200, body: experimentPageJson() });
    research.on('GET', `${RESEARCH_API}/runs`, { status: 200, body: { runs: [], next_cursor: null } });
    research.on('GET', `${RESEARCH_API}/results`, { status: 200, body: { results: [], invalid_entries: [], next_cursor: null } });
    renderWithServices(<App />, { api, research });
    expect(await screen.findByRole('heading', { name: 'Order ticket' })).toBeInTheDocument();

    navigate('#/research');
    expect(await screen.findByRole('heading', { name: 'Historical evaluation' })).toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'Research' })).toHaveAttribute('aria-current', 'page');
    expect(screen.getByRole('banner')).toHaveTextContent(ACCOUNT_ID);
    // Research reads nothing from, and writes nothing to, the trading API.
    expect(api.requests.filter((request) => request.method !== 'GET')).toHaveLength(0);
    expect(await screen.findByText('No results have been published yet.')).toBeInTheDocument();

    navigate('#/');
    expect(await screen.findByRole('heading', { name: 'Order ticket' })).toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'Trading' })).toHaveAttribute('aria-current', 'page');
    expect(window.localStorage.getItem(SELECTED_ACCOUNT_KEY)).toBe(ACCOUNT_ID);
  });

  it('keeps workspace section links in the trading view', async () => {
    rememberSelectedAccount();
    const api = createFakeApi();
    serveAccount(api);
    renderWithServices(<App />, { api });
    expect(await screen.findByRole('heading', { name: 'Order ticket' })).toBeInTheDocument();
    navigate('#portfolio');
    expect(screen.getByRole('heading', { name: 'Order ticket' })).toBeInTheDocument();
  });
});
