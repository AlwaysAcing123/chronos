import { screen, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { AppShell } from '../shell/AppShell';
import { parseRoute } from '../../state/route';
import { DEMO_IDS, demoGuideJson } from '../../test/demoFixtures';
import { createFakeApi } from '../../test/fakeApi';
import { createFakeResearch } from '../../test/fakeResearch';
import type { Json } from '../../test/fixtures';
import { marketDataJson, syntheticSourceJson } from '../../test/fixtures';
import { renderWithServices, serveServer } from '../../test/renderWithServices';
import { DemoGuide } from './DemoGuide';

function renderGuide({
  guide = demoGuideJson(),
  selected = null,
  status = 200,
}: { guide?: Json; selected?: string | null; status?: number } = {}) {
  const api = createFakeApi();
  serveServer(api, marketDataJson({ source: syntheticSourceJson() }));
  const research = createFakeResearch();
  research.on('GET', '/api/v1/research/demo', { status, body: guide });
  const view = renderWithServices(<DemoGuide selectedAccountId={selected} />, { api, research, demo: true });
  return { ...view, research };
}

function linkTo(name: string): string | null {
  return screen.getAllByRole('link', { name }).at(0)?.getAttribute('href') ?? null;
}

describe('demo guide', () => {
  it('guides three flows through links to the seeded artifacts, and starts nothing', async () => {
    const { api, research } = renderGuide();
    expect(await screen.findByRole('heading', { name: '1. Manual paper trading' })).toBeInTheDocument();
    expect(screen.getByRole('heading', { name: '2. Evaluate, then paper-deploy a model' })).toBeInTheDocument();
    expect(screen.getByRole('heading', { name: '3. Explain a strategy result' })).toBeInTheDocument();
    expect(linkTo('logistic model')).toBe(`#/models/${DEMO_IDS.logistic}`);
    expect(linkTo('majority baseline')).toBe(`#/models/${DEMO_IDS.majority}`);
    expect(linkTo('comparison')).toBe(`#/comparisons/${DEMO_IDS.comparison}`);
    expect(linkTo('AAPL walk-forward study')).toBe(`#/research/walk-forward/studies/${DEMO_IDS.studyAapl}`);
    expect(linkTo('the first fold’s logistic base run')).toBe(
      `#/research/walk-forward/diagnostics/${DEMO_IDS.diagnostics}/runs/${DEMO_IDS.run}/strategy`,
    );
    // It explains and links; it has no control of its own.
    expect(screen.queryAllByRole('button')).toEqual([]);
    expect([...api.requests, ...research.requests].every((request) => request.method === 'GET')).toBe(true);
    expect(research.requests.map((request) => request.path)).toEqual(['/api/v1/research/demo']);
  });

  it('says the data is synthetic and the replay waits for an explicit Start', async () => {
    renderGuide();
    expect(await screen.findByText('Every price here is invented')).toBeInTheDocument();
    const session = screen.getByText('Session 1: fresh playback').closest('div');
    expect(session).not.toBeNull();
    expect(within(session as HTMLElement).getByText(/This guide never presses it/)).toBeInTheDocument();
    expect(await within(session as HTMLElement).findByText('Ready to start')).toBeInTheDocument();
    expect(screen.getByText('Displayed liquidity:', { exact: false })).toHaveTextContent(
      'removes no real shares from any book',
    );
    expect(screen.getByText(/Stop never sells/)).toBeInTheDocument();
  });

  it('explains a retained session as history, with no replay', async () => {
    renderGuide({ guide: demoGuideJson({ mode: 'history', number: '2' }) });
    expect(await screen.findByText('Session 2: history, with no replay')).toBeInTheDocument();
    expect(screen.getByText(/valuation is unavailable rather than zero/)).toBeInTheDocument();
    expect(screen.getByText('new-session')).toBeInTheDocument();
  });

  it('keeps the selected paper account and says so', async () => {
    renderGuide({ selected: 'demo-manual-1' });
    expect(await screen.findByText(/The account you had open, demo-manual-1, is still selected/)).toBeInTheDocument();
    expect(linkTo('Trading')).toBe('#/');
  });

  it('names an unavailable run rather than linking to it', async () => {
    renderGuide({ guide: demoGuideJson({ diagnosticsRun: { unavailable: 'run_failed' } }) });
    expect(await screen.findByText(/which is unavailable \(run_failed\)/)).toBeInTheDocument();
    expect(screen.queryByRole('link', { name: 'the first fold’s logistic base run' })).not.toBeInTheDocument();
  });

  it('reports guide data that cannot be read, and refuses anything but application routes', async () => {
    renderGuide({
      status: 422,
      guide: { error: { code: 'artifact_invalid', message: 'demo seed does not verify', field: null, reason: null } },
    });
    expect(await screen.findByText("The guide's data could not be read")).toBeInTheDocument();
    expect(screen.queryByRole('heading', { name: '1. Manual paper trading' })).not.toBeInTheDocument();
  });

  it('refuses a route to another origin', async () => {
    const guide = demoGuideJson();
    const seed = guide.seed as Json;
    seed.routes = { ...(seed.routes as Json), comparison: 'https://example.invalid/' };
    renderGuide({ guide });
    expect(await screen.findByText("The guide's data could not be read")).toBeInTheDocument();
  });
});

describe('an ordinary build', () => {
  it('has no guide route and no Guide link', () => {
    expect(parseRoute('#/demo')).toEqual({ view: 'trading' });
    expect(parseRoute('#/demo', false)).toEqual({ view: 'trading' });
    expect(parseRoute('#/demo', true)).toEqual({ view: 'demo' });
    expect(parseRoute('#/demo/flows', true)).toEqual({ view: 'demo' });
    expect(parseRoute('#/research', true)).toEqual({ view: 'research', resultId: null });
    renderWithServices(<AppShell accountContext={null}>content</AppShell>);
    const navigation = screen.getByRole('navigation', { name: 'Primary' });
    expect(within(navigation).getAllByRole('link').map((link) => link.textContent)).toEqual([
      'Trading',
      'Models',
      'Comparisons',
      'Research',
    ]);
  });
});
