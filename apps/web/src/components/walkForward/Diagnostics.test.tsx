import { fireEvent, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';

import { RESEARCH_API } from '../../api/research';
import { SELECTED_ACCOUNT_KEY } from '../../state/accountSelection';
import { parseRoute } from '../../state/route';
import {
  DIAGNOSED_RUN_ID,
  DIAGNOSED_STUDY_ID,
  DIAGNOSTICS_JOB_ID,
  DIAGNOSTIC_ID,
  diagnosticsDetailJson,
  diagnosticsJobJson,
  diagnosticsPageJson,
  emptyDiagnosticsPageJson,
  episodePageJson,
  fillPageJson,
} from '../../test/diagnosticsFixtures';
import { createFakeResearch } from '../../test/fakeResearch';
import type { FakeResearch } from '../../test/fakeResearch';
import type { Json } from '../../test/fixtures';
import { rememberSelectedAccount, renderWithServices } from '../../test/renderWithServices';
import { DiagnosticsDetailView } from './DiagnosticsDetail';
import { StudyDiagnostics } from './StudyDiagnostics';
import { WalkForward } from './WalkForward';

const DETAIL_PATH = `${RESEARCH_API}/strategy-diagnostics/${DIAGNOSTIC_ID}`;

function serveDiagnostics(research: FakeResearch, detail: Json = diagnosticsDetailJson()) {
  research.on('GET', DETAIL_PATH, { status: 200, body: detail });
  research.on('GET', `${DETAIL_PATH}/fills`, (request) => ({
    status: 200,
    body:
      request.query.get('account') === 'buy_and_hold'
        ? fillPageJson('buy_and_hold')
        : request.query.get('account') === 'strategy' && request.query.get('run') === DIAGNOSED_RUN_ID
          ? request.query.get('cursor') === null
            ? fillPageJson('first')
            : fillPageJson('next')
          : { fills: [], next_cursor: null },
  }));
  research.on('GET', `${DETAIL_PATH}/episodes`, (request) => ({
    status: 200,
    body:
      request.query.get('account') === 'strategy' && request.query.get('run') === DIAGNOSED_RUN_ID
        ? episodePageJson()
        : { episodes: [], next_cursor: null },
  }));
}

function first(items: unknown): Json {
  const item = (items as Json[])[0];
  if (item === undefined) {
    throw new Error('the fixture has no such entry');
  }
  return item;
}

// The fixture's run of one model and scenario.
function runOf(detail: Json, model: string, scenario: string): Json {
  const fold = first(first((detail.diagnostics as Json).studies).folds);
  const run = (fold.runs as Json[]).find((entry) => entry.model === model && entry.scenario === scenario);
  if (run === undefined) {
    throw new Error('no such fixture run');
  }
  return run;
}

afterEach(() => {
  window.location.hash = '';
});

describe('Strategy diagnostics', () => {
  it('opens from its deep link and keeps the simulated account apart from the paper account', () => {
    expect(parseRoute(`#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID}`)).toEqual({
      view: 'diagnostics',
      diagnosticId: DIAGNOSTIC_ID,
      runId: null,
      account: 'strategy',
    });
    expect(parseRoute(`#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID}/runs/${DIAGNOSED_RUN_ID}/cash`)).toEqual({
      view: 'diagnostics',
      diagnosticId: DIAGNOSTIC_ID,
      runId: DIAGNOSED_RUN_ID,
      account: 'cash',
    });
    // Anything that is not an ID or an account opens the walk-forward home, never a path.
    for (const hash of [
      '#/research/walk-forward/diagnostics/..%2F..%2Fetc',
      `#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID}/runs/${DIAGNOSED_RUN_ID}/paper-1`,
      `#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID.toUpperCase()}`,
    ]) {
      expect(parseRoute(hash)).toEqual({ view: 'walk-forward', planId: null, studyId: null });
    }
  });

  it('shows every verification level apart, then each run, the pools, and the definitions', async () => {
    const research = createFakeResearch();
    serveDiagnostics(research);
    renderWithServices(<WalkForward planId={null} studyId={null} diagnostics={{ diagnosticId: DIAGNOSTIC_ID, runId: null, account: 'strategy' }} />, {
      research,
    });

    expect(await screen.findByRole('heading', { name: 'Strategy diagnostics', level: 1 })).toBeInTheDocument();
    expect(await screen.findByText(DIAGNOSTIC_ID)).toHaveClass(/ids/);
    expect(screen.getByText('Synthetic recordings', { selector: 'p' })).toBeInTheDocument();
    expect(screen.getByText('An exact explanation, not evidence of skill')).toBeInTheDocument();

    const levels = screen.getByRole('table', { name: 'Each level of verification and its state' });
    const rows = within(levels).getAllByRole('row').slice(1);
    expect(rows.map((row) => row.querySelector('td[data-label="Level"]')?.textContent)).toEqual([
      'Artifact checksums',
      'Every source, checked on this read',
      'Sources at generation',
      'Recomputed inspection',
      'Independent arithmetic audit',
      'Engine replay',
    ]);
    // No single badge stands for every level.
    expect(within(levels).getByText('Verified on this load')).toBeInTheDocument();
    expect(within(levels).getAllByText('Not run by this page')).toHaveLength(2);
    expect(within(levels).getByText('Recorded by the study')).toBeInTheDocument();
    expect(within(levels).queryByText('Verified')).toBeNull();
    expect(within(levels).getByText(new RegExp(`audit.py <artifacts> ${DIAGNOSTIC_ID}`))).toBeInTheDocument();
    expect(within(levels).getByText(/9 files hashed now and 0 reused unchanged/)).toBeInTheDocument();
    // Every kind of source the read checked, counted apart.
    const kinds = levels.closest('section') ?? document.body;
    for (const [kind, count] of [
      ['Studies', 1],
      ['Plans', 1],
      ['Collections', 1],
      ['Datasets', 1],
      ['Feature artifacts', 1],
      ['Label artifacts', 1],
      ['Schedules', 2],
      ['Run journals', 4],
    ] as const) {
      expect(within(kinds).getByText(kind).nextElementSibling).toHaveTextContent(`${String(count)} of ${String(count)} verified`);
    }

    const runs = screen.getByRole('table', { name: /run, explained/ });
    const runRows = within(runs).getAllByRole('row').slice(1);
    expect(runRows).toHaveLength(4);
    const logistic = runRows.find((row) => row.textContent.includes('Logistic regression · Base'));
    expect(logistic).toBeDefined();
    if (logistic === undefined) {
      return;
    }
    // The service's exact strings, not recomputed here.
    expect(logistic.querySelector('td[data-label="Booked net PnL"]')).toHaveTextContent('-0.81');
    expect(logistic.querySelector('td[data-label="Spread paid at execution"]')).toHaveTextContent('0.37');
    expect(within(logistic).getByText('Reconciles exactly')).toBeInTheDocument();
    expect(within(logistic).getByRole('link', { name: /^Explain/ })).toHaveAttribute(
      'href',
      `#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID}/runs/${DIAGNOSED_RUN_ID}/strategy`,
    );
    for (const row of runRows) {
      for (const cell of within(row).getAllByRole('cell')) {
        expect(cell).toHaveAttribute('data-label');
      }
    }
    const pools = screen.getByRole('table', { name: 'AAPL attribution pooled over folds' });
    expect(within(pools).getAllByText('All folds').length).toBeGreaterThan(0);
    expect(within(pools).getAllByRole('cell', { name: 'Strategy − cash' })).toHaveLength(4);
    expect(screen.getByText(/booked_net_pnl = midpoint_inventory_pnl - execution_spread_cost/)).toBeInTheDocument();
    // Every scroller is reachable with the keyboard and named.
    for (const scroller of document.querySelectorAll('[role="group"][tabindex="0"]')) {
      expect(scroller).toHaveAccessibleName(/scrollable$/);
    }
  });

  it('explains one run term by term, with its decisions, episodes, and fills', async () => {
    const research = createFakeResearch();
    serveDiagnostics(research);
    const { user } = renderWithServices(
      <DiagnosticsDetailView diagnosticId={DIAGNOSTIC_ID} runId={DIAGNOSED_RUN_ID} account="strategy" />,
      { research },
    );

    const selected = await screen.findByRole('region', { name: /^AAPL fold 1 · Logistic regression · Base · Strategy/ });
    const bridge = within(selected).getByRole('table', { name: 'How the booked net PnL arises, term by term' });
    const terms = within(bridge)
      .getAllByRole('row')
      .slice(1)
      .map((row) => [row.querySelector('td[data-label="Term"]')?.textContent, row.querySelectorAll('td')[1]?.textContent]);
    expect(terms).toEqual([
      ['Midpoint inventory PnL', '-0.06'],
      ['− Spread paid at execution', '0.37'],
      ['− Fees', '0.37'],
      ['− Final mark at the bid, not the midpoint', '0.01'],
      ['+ Rounding adjustment', '0'],
      ['= Booked net PnL', '-0.81'],
    ]);
    expect(within(selected).getAllByText('Reconciles exactly')).toHaveLength(2);
    const signal = within(selected).getByRole('table', { name: 'The midpoint inventory PnL from the signal midpoints' });
    expect(within(signal).getByRole('cell', { name: '− Midpoint move, signal to execution' })).toBeInTheDocument();

    const cohorts = within(selected).getByRole('table', { name: 'Decision cohorts of this run' });
    expect(Array.from(cohorts.querySelectorAll('td[data-label="Cohort"]')).map((cell) => cell.textContent)).toEqual([
      'Scorable predictions',
      'Sampled decisions',
      'Order-scheduling decisions',
      'Orders with fills',
    ]);
    const sampled = within(cohorts).getByRole('cell', { name: 'Sampled decisions' }).closest('tr');
    expect(sampled?.querySelector('td[data-label="Accuracy"]')).toHaveTextContent('0.4489822 / 49');
    const bins = within(selected).getByRole('table', { name: /Fixed confidence bins/ });
    expect(within(bins).getAllByRole('row')).toHaveLength(11);
    // Future returns in the labels' own unit, exactly as the service gave them.
    const returns = within(selected).getByRole('table', { name: 'Future midpoint returns of this run’s scored cohort items' });
    const sampledReturns = within(returns).getByRole('cell', { name: 'Sampled decisions' }).closest('tr');
    expect(sampledReturns?.querySelector('td[data-label="Scored · not scored"]')).toHaveTextContent('98 · 1');
    expect(sampledReturns?.querySelector('td[data-label="Mean"]')).toHaveTextContent('-8144.887755 ppb-798199 / 98 rows');
    expect(sampledReturns?.querySelector('td[data-label="Minimum"]')).toHaveTextContent('-400160 ppb');
    expect(sampledReturns?.querySelector('td[data-label="After an up prediction"]')).toHaveTextContent('-5244.736842 ppb');
    expect(sampledReturns?.querySelector('td[data-label="After a flat prediction"]')).toHaveTextContent(
      'Undefined: Nothing scored',
    );
    const binRow = within(bins).getByRole('cell', { name: '0.4-0.5' }).closest('tr');
    expect(binRow).toHaveTextContent('ppb');
    expect(within(selected).getByText(/parts per billion, rounded half away from zero by the label generator/)).toBeInTheDocument();

    const episodes = await within(selected).findByRole('table', { name: 'Every holding episode of this account' });
    expect(within(episodes).getAllByText('Completed')).toHaveLength(18);
    expect(within(episodes).getByText('Censored at the window’s end')).toBeInTheDocument();

    const fills = await within(selected).findByRole('table', { name: /Every fill of this account/ });
    expect(within(fills).getAllByRole('row')).toHaveLength(5);
    await user.click(within(selected).getByRole('button', { name: /Load more fills/ }));
    expect(await within(fills).findByText('strategy-000005')).toBeInTheDocument();
    const cursors = research.requestsTo('GET', `${DETAIL_PATH}/fills`).map((request) => request.query.get('cursor'));
    expect(cursors[0]).toBeNull();
    expect(cursors.at(-1)).not.toBeNull();
    // Reads only: nothing was posted.
    expect(research.requests.filter((request) => request.method !== 'GET')).toEqual([]);
  });

  it('switches fold, model, scenario, and simulated account by deep link, never the paper account', async () => {
    rememberSelectedAccount('paper-account-7');
    const research = createFakeResearch();
    serveDiagnostics(research);
    renderWithServices(<DiagnosticsDetailView diagnosticId={DIAGNOSTIC_ID} runId={DIAGNOSED_RUN_ID} account="strategy" />, {
      research,
    });

    const selected = await screen.findByRole('region', { name: /Logistic regression · Base · Strategy/ });
    const accounts = within(selected).getByRole('navigation', { name: 'Simulated account' });
    expect(within(accounts).getByRole('link', { name: 'Strategy (shown)' })).toHaveAttribute('aria-current', 'page');
    expect(within(accounts).getByRole('link', { name: 'Cash' })).toHaveAttribute(
      'href',
      `#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID}/runs/${DIAGNOSED_RUN_ID}/cash`,
    );
    const detail = diagnosticsDetailJson();
    const stress = runOf(detail, 'logistic', 'stress');
    fireEvent.change(within(selected).getByLabelText('Scenario'), { target: { value: 'stress' } });
    expect(window.location.hash).toBe(
      `#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID}/runs/${String(stress.run_id)}/strategy`,
    );
    const majority = runOf(detail, 'majority', 'base');
    fireEvent.change(within(selected).getByLabelText('Model'), { target: { value: 'majority' } });
    expect(window.location.hash).toBe(
      `#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID}/runs/${String(majority.run_id)}/strategy`,
    );
    expect(within(selected).getByLabelText('Fold')).toHaveDisplayValue(/^Fold 1 · synthetic-2026-09-23/);
    expect(window.localStorage.getItem(SELECTED_ACCOUNT_KEY)).toBe('paper-account-7');
  });

  it('shows genuine zeros for a model that never traded and the benchmark’s own fills', async () => {
    const research = createFakeResearch();
    serveDiagnostics(research);
    const detail = diagnosticsDetailJson();
    const majority = runOf(detail, 'majority', 'base');
    renderWithServices(<DiagnosticsDetailView diagnosticId={DIAGNOSTIC_ID} runId={String(majority.run_id)} account="strategy" />, {
      research,
    });
    const selected = await screen.findByRole('region', { name: /Majority class \(no skill\) · Base · Strategy/ });
    expect(await within(selected).findByText(/no fill, so every execution term is a genuine zero/)).toBeInTheDocument();
    expect(within(selected).getByText('This account never held a position.')).toBeInTheDocument();
    const bridge = within(selected).getByRole('table', { name: 'How the booked net PnL arises, term by term' });
    for (const cell of within(bridge).getAllByRole('cell', { name: /^0$/ })) {
      expect(cell).toHaveAttribute('data-label');
    }
  });

  it('names every missing figure with its reason and never shows it as zero', async () => {
    const research = createFakeResearch();
    const detail = diagnosticsDetailJson();
    const run = runOf(detail, 'logistic', 'base');
    const strategy = (run.accounts as Json).strategy as Json;
    const reason = 'final_valuation_unavailable:position_mark_unavailable';
    const components = strategy.components as Json;
    for (const name of ['booked_net_pnl', 'midpoint_inventory_pnl', 'terminal_bid_markdown', 'rounding_adjustment']) {
      components[name] = { unavailable: reason };
    }
    strategy.final_mark = { unavailable: reason };
    strategy.reconciliation = { booked_net_pnl: { unavailable: reason }, signal_reference: { unavailable: reason } };
    serveDiagnostics(research, detail);
    renderWithServices(<DiagnosticsDetailView diagnosticId={DIAGNOSTIC_ID} runId={DIAGNOSED_RUN_ID} account="strategy" />, {
      research,
    });
    const selected = await screen.findByRole('region', { name: /Logistic regression · Base · Strategy/ });
    const bridge = within(selected).getByRole('table', { name: 'How the booked net PnL arises, term by term' });
    const booked = within(bridge).getByRole('cell', { name: '= Booked net PnL' }).closest('tr');
    expect(booked).toHaveTextContent('Unavailable: The final valuation is unavailable: the position could not be marked');
    expect(booked).toHaveTextContent(reason);
    expect(booked?.querySelectorAll('td')[1]).not.toHaveTextContent(/^0$/);
    expect(within(selected).getAllByText('Not checkable')).toHaveLength(2);
    expect(within(bridge).getByRole('cell', { name: '− Spread paid at execution' }).closest('tr')).toHaveTextContent('0.37');
  });

  it('labels runs whose sources no longer match, and lists a partial artifact’s missing runs', async () => {
    const research = createFakeResearch();
    const detail = diagnosticsDetailJson('run');
    const diagnostics = detail.diagnostics as Json;
    diagnostics.status = 'partial';
    const study = first(diagnostics.studies);
    study.status = 'partial';
    study.missing = [{ fold: '1', item: 'cell:majority:stress', reason: 'cell_failed:run_failed' }];
    const fold = first(study.folds);
    fold.runs = (fold.runs as Json[]).map((run) =>
      run.model === 'majority' && run.scenario === 'stress'
        ? { fold: '1', model: 'majority', reason: 'cell_failed:run_failed', scenario: 'stress', state: 'unavailable' }
        : run,
    );
    serveDiagnostics(research, detail);
    renderWithServices(<DiagnosticsDetailView diagnosticId={DIAGNOSTIC_ID} runId={DIAGNOSED_RUN_ID} account="strategy" />, {
      research,
    });
    const alert = await screen.findByText('Some sources no longer match what the diagnostics read');
    expect(alert.parentElement).toHaveTextContent(
      `Run journal ${DIAGNOSED_RUN_ID.slice(0, 12)}: invalid: journal.ndjson differs`,
    );
    expect(screen.getByText('Some sources of this run no longer match what the diagnostics read')).toBeInTheDocument();
    expect(screen.getAllByText('Unverified run').length).toBeGreaterThan(0);
    expect(screen.getByText('These diagnostics are partial').parentElement).toHaveTextContent(
      'AAPL fold 1, cell majority stress: Its run failed: the strategy run failed',
    );
    const runs = screen.getByRole('table', { name: /run, explained/ });
    expect(within(runs).getByText('Not attributed')).toBeInTheDocument();
    expect(within(screen.getByRole('table', { name: 'Each level of verification and its state' })).getByText('Do not match')).toBeInTheDocument();
  });

  // What the service returned with one source changed: each figure resting on
  // it is labelled, and nothing else is.
  it.each([
    ['run', ['Logistic regression · Base'], ['Logistic regression|base']],
    ['schedule', ['Logistic regression · Base', 'Logistic regression · Stress'], ['Logistic regression|base', 'Logistic regression|stress']],
    [
      'study',
      ['Logistic regression · Base', 'Logistic regression · Stress', 'Majority class (no skill) · Base', 'Majority class (no skill) · Stress'],
      ['Logistic regression|base', 'Logistic regression|stress', 'Majority class (no skill)|base', 'Majority class (no skill)|stress'],
    ],
  ] as const)('labels every run, cohort, and pool resting on an invalid %s, and only those', async (kind, unverifiedRuns, unverifiedPools) => {
    const research = createFakeResearch();
    serveDiagnostics(research, diagnosticsDetailJson(kind));
    renderWithServices(<DiagnosticsDetailView diagnosticId={DIAGNOSTIC_ID} runId={DIAGNOSED_RUN_ID} account="strategy" />, {
      research,
    });
    const selected = await screen.findByRole('region', { name: /^AAPL fold 1 · Logistic regression · Base · Strategy/ });
    expect(within(selected).getAllByText('Unverified run')).toHaveLength(1);
    // A recorded identity says only that it held when generated.
    expect(within(selected).queryByText('Reconciles exactly')).toBeNull();
    expect(within(selected).getAllByText('Reconciled when generated')).toHaveLength(2);
    expect(within(selected).getAllByText('Recorded, unverified').length).toBeGreaterThanOrEqual(5);
    const cohorts = within(selected).getByRole('table', { name: 'Decision cohorts of this run' });
    expect(within(cohorts).getAllByText('Unverified cohort')).toHaveLength(kind === 'run' ? 3 : 4);

    const runRows = within(screen.getByRole('table', { name: /run, explained/ })).getAllByRole('row').slice(1);
    for (const row of runRows) {
      const name = row.querySelector('td[data-label="Run"]')?.textContent ?? '';
      const expected = unverifiedRuns.some((run) => name.includes(run));
      expect(within(row).queryByText('Unverified run') !== null, name).toBe(expected);
      expect(within(row).queryByText('Reconciles exactly') !== null, name).toBe(!expected);
    }
    const pools = within(screen.getByRole('table', { name: 'AAPL attribution pooled over folds' })).getAllByRole('row').slice(1);
    for (const row of pools) {
      const model = row.querySelector('td[data-label="Model"]')?.textContent ?? '';
      const scenario = (row.querySelector('td[data-label="Scenario"]')?.textContent ?? '').toLowerCase();
      const expected = (unverifiedPools as readonly string[]).includes(`${model}|${scenario}`);
      expect(within(row).queryByText('Unverified pool') !== null, `${model} ${scenario}`).toBe(expected);
    }
    const pooledCohorts = screen.getByRole('table', { name: 'AAPL cohorts pooled over folds' });
    expect(within(pooledCohorts).getAllByText('Unverified cohort').length).toBeGreaterThan(0);
  });

  it('shows a pool with no completed run as unavailable, never as a zero', async () => {
    const research = createFakeResearch();
    const detail = diagnosticsDetailJson();
    const study = first((detail.diagnostics as Json).studies);
    const stress = ((study.pooled as Json).trading as Json).logistic as Json;
    const none = { unavailable: 'no_completed_runs' };
    const terms = Object.fromEntries(
      Object.keys(((stress.stress as Json).accounts as Json).strategy as Json).map((name) => [name, none]),
    );
    stress.stress = {
      ...(stress.stress as Json),
      accounts: { strategy: terms, buy_and_hold: terms, cash: terms },
      deltas: { buy_and_hold: terms, cash: terms },
      folds: { included: [], missing: ['1'], planned: '1' },
      status: 'unavailable',
    };
    serveDiagnostics(research, detail);
    renderWithServices(<DiagnosticsDetailView diagnosticId={DIAGNOSTIC_ID} runId={null} account="strategy" />, { research });
    const pools = await screen.findByRole('table', { name: 'AAPL attribution pooled over folds' });
    const rows = within(pools)
      .getAllByRole('row')
      .slice(1)
      .filter(
        (row) =>
          row.querySelector('td[data-label="Model"]')?.textContent === 'Logistic regression' &&
          row.querySelector('td[data-label="Scenario"]')?.textContent === 'Stress',
      );
    expect(rows).toHaveLength(5);
    for (const row of rows) {
      expect(within(row).getByText('No fold')).toBeInTheDocument();
      const booked = row.querySelector('td[data-label="Booked net PnL"]');
      expect(booked).toHaveTextContent('Unavailable: No run of this pool completed, so nothing was evaluated');
      expect(booked).not.toHaveTextContent(/^-?0$/);
    }
  });

  it.each([
    [404, { error: { code: 'diagnostics_not_found', message: 'no such strategy diagnostics' } }],
    [422, { error: { code: 'artifact_invalid', message: 'the diagnostics cannot be read: fills.ndjson differs' } }],
  ])('says plainly when diagnostics are missing or invalid (%i)', async (status, body) => {
    const research = createFakeResearch();
    research.on('GET', DETAIL_PATH, { status, body });
    renderWithServices(<DiagnosticsDetailView diagnosticId={DIAGNOSTIC_ID} runId={null} account="strategy" />, { research });
    expect(await screen.findByText('The diagnostics could not be read')).toBeInTheDocument();
    expect(screen.queryByRole('table')).toBeNull();
  });

  it('shows a loading state, and a deep link to a run that is not in the artifact', async () => {
    const research = createFakeResearch();
    serveDiagnostics(research);
    renderWithServices(<DiagnosticsDetailView diagnosticId={DIAGNOSTIC_ID} runId={'f'.repeat(64)} account="cash" />, {
      research,
    });
    expect(screen.getByRole('status')).toHaveTextContent('Reading and verifying the diagnostics…');
    expect(await screen.findByText('That run is not in these diagnostics')).toBeInTheDocument();
  });
});

describe('A study’s diagnostics', () => {
  it('lists none, and generates only when asked, following the job to its diagnostics', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/strategy-diagnostics`, () => ({
      status: 200,
      body: research.requestsTo('POST', `${RESEARCH_API}/strategy-diagnostics`).length === 0 ? emptyDiagnosticsPageJson() : diagnosticsPageJson(),
    }));
    research.on('POST', `${RESEARCH_API}/strategy-diagnostics`, {
      status: 202,
      body: { deduplicated: false, job: diagnosticsJobJson({ state: 'running', result: null, finished_at: null, progress: { stage: 'attribute', fold: '1', folds: '1', model: 'logistic', scenario: 'stress' } }) },
    });
    let polls = 0;
    research.on('GET', `${RESEARCH_API}/jobs/${DIAGNOSTICS_JOB_ID}`, () => {
      polls += 1;
      return {
        status: 200,
        body: {
          job:
            polls === 1
              ? diagnosticsJobJson({ state: 'running', result: null, finished_at: null, progress: { stage: 'attribute', fold: '1', folds: '1', model: 'logistic', scenario: 'stress' } })
              : diagnosticsJobJson(),
        },
      };
    });
    const { user } = renderWithServices(<StudyDiagnostics studyId={DIAGNOSED_STUDY_ID} />, { research });
    const panel = await screen.findByRole('region', { name: 'Strategy diagnostics' });
    expect(await within(panel).findByText('No diagnostics of this study exist yet.')).toBeInTheDocument();
    const listed = research.requestsTo('GET', `${RESEARCH_API}/strategy-diagnostics`);
    expect(listed[0]?.query.get('study')).toBe(DIAGNOSED_STUDY_ID);
    expect(research.requestsTo('POST', `${RESEARCH_API}/strategy-diagnostics`)).toEqual([]);

    await user.click(within(panel).getByRole('button', { name: 'Explain this study’s runs' }));
    expect(research.requestsTo('POST', `${RESEARCH_API}/strategy-diagnostics`).map((request) => request.body)).toEqual([
      { study_ids: [DIAGNOSED_STUDY_ID] },
    ]);
    expect(await within(panel).findByText('Attributing runs: fold 1 of 1, Logistic regression, Stress')).toBeInTheDocument();
    expect(await within(panel).findByText('Diagnostics published', {}, { timeout: 4000 })).toBeInTheDocument();
    expect(within(panel).getByRole('link', { name: 'Open the diagnostics' })).toHaveAttribute(
      'href',
      `#/research/walk-forward/diagnostics/${DIAGNOSTIC_ID}`,
    );
    expect(await within(panel).findByRole('link', { name: `Diagnostics ${DIAGNOSTIC_ID.slice(0, 12)}…` })).toBeInTheDocument();
  });

  it('reports a refused job with its reason', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/strategy-diagnostics`, { status: 200, body: emptyDiagnosticsPageJson() });
    research.on('POST', `${RESEARCH_API}/strategy-diagnostics`, {
      status: 200,
      body: { deduplicated: true, job: diagnosticsJobJson({ state: 'queued', result: null, finished_at: null, progress: null }) },
    });
    research.on('GET', `${RESEARCH_API}/jobs/${DIAGNOSTICS_JOB_ID}`, {
      status: 200,
      body: {
        job: diagnosticsJobJson({
          state: 'failed',
          result: null,
          error: { code: 'diagnostics_refused', message: 'fold 1: run x is not its cell’s', reason: 'source_invalid' },
        }),
      },
    });
    const { user } = renderWithServices(<StudyDiagnostics studyId={DIAGNOSED_STUDY_ID} />, { research });
    const panel = await screen.findByRole('region', { name: 'Strategy diagnostics' });
    await user.click(await within(panel).findByRole('button', { name: 'Explain this study’s runs' }));
    expect(await within(panel).findByText('Diagnostics failed')).toBeInTheDocument();
    expect(within(panel).getByText('source_invalid')).toBeInTheDocument();
    expect(within(panel).getByText('The same diagnostics were already requested; this is that job.')).toBeInTheDocument();
  });

  it('says when the list cannot be read', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/strategy-diagnostics`, {
      status: 422,
      body: { error: { code: 'artifact_invalid', message: 'the diagnostics root is not a real directory' } },
    });
    renderWithServices(<StudyDiagnostics studyId={DIAGNOSED_STUDY_ID} />, { research });
    expect(await screen.findByText('The diagnostics could not be listed')).toBeInTheDocument();
    // Unreachable is said as such.
    const unreachable = createFakeResearch();
    unreachable.failNext('GET', `${RESEARCH_API}/strategy-diagnostics`);
    renderWithServices(<StudyDiagnostics studyId={DIAGNOSED_STUDY_ID} />, { research: unreachable });
    expect(await screen.findByText('The research service is not reachable')).toBeInTheDocument();
  });
});
