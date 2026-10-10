import { screen, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { RESEARCH_API } from '../../api/research';
import { createFakeResearch } from '../../test/fakeResearch';
import type { FakeResearch } from '../../test/fakeResearch';
import type { Json } from '../../test/fixtures';
import { renderWithServices } from '../../test/renderWithServices';
import { DEFAULT_CONFIGURATION, researchStatusJson } from '../../test/researchFixtures';
import {
  COMPLETE_STUDY_ID,
  FOLD_RUN_ID,
  ONE_RECORDING_COLLECTION_ID,
  PARTIAL_STUDY_ID,
  PLAN_ID,
  PLAN_JOB_ID,
  STUDY_JOB_ID,
  WEEK_COLLECTION_ID,
  collectionJson,
  collectionPageJson,
  failedPlanJobJson,
  foldResultJson,
  partialStudyDetailJson,
  planJobJson,
  planJson,
  studyJobJson,
  studyJobPageJson,
  studyPageJson,
} from '../../test/walkForwardFixtures';
import { ResultView } from '../research/ResultView';
import { WalkForward } from './WalkForward';

function serveHome(research: FakeResearch, collections: Json = collectionPageJson([collectionJson(WEEK_COLLECTION_ID)])) {
  research.on('GET', `${RESEARCH_API}/status`, { status: 200, body: researchStatusJson() });
  research.on('GET', `${RESEARCH_API}/collections`, { status: 200, body: collections });
  research.on('GET', `${RESEARCH_API}/walk-forward/studies`, { status: 200, body: studyPageJson() });
  research.on('GET', `${RESEARCH_API}/jobs`, { status: 200, body: studyJobPageJson([]) });
}

describe('Walk-forward evaluation', () => {
  it('lists verified collections with their declared sessions and partial coverage', async () => {
    const research = createFakeResearch();
    serveHome(
      research,
      collectionPageJson([collectionJson(WEEK_COLLECTION_ID), collectionJson(ONE_RECORDING_COLLECTION_ID)]),
    );
    renderWithServices(<WalkForward planId={null} studyId={null} />, { research });

    const list = await screen.findByRole('region', { name: 'Recording collections' });
    const week = await within(list).findByRole('table', { name: new RegExp(WEEK_COLLECTION_ID) });
    expect(within(week).getAllByRole('row')).toHaveLength(4);
    expect(within(week).getByRole('cell', { name: /^synthetic-2026-09-21/ })).toBeInTheDocument();
    expect(within(week).getAllByText('No session label')).toHaveLength(3);
    expect(within(week).getAllByText(/partial coverage/)).toHaveLength(3);
    expect(within(list).getAllByText('Synthetic recordings').length).toBeGreaterThan(0);
    // Every figure on a phone card names what it is.
    for (const cell of within(week).getAllByRole('cell')) {
      expect(cell).toHaveAttribute('data-label');
    }
    // A study's jobs and its published studies are on the same page.
    expect(await screen.findByRole('region', { name: 'Published studies' })).toBeInTheDocument();
    expect(screen.getByRole('region', { name: 'Study jobs' })).toBeInTheDocument();
  });

  it('lets every collection’s member table scroll with the keyboard, as real stream names widen it', async () => {
    const research = createFakeResearch();
    serveHome(
      research,
      collectionPageJson([collectionJson(WEEK_COLLECTION_ID), collectionJson(ONE_RECORDING_COLLECTION_ID)]),
    );
    renderWithServices(<WalkForward planId={null} studyId={null} />, { research });

    const list = await screen.findByRole('region', { name: 'Recording collections' });
    for (const id of [WEEK_COLLECTION_ID, ONE_RECORDING_COLLECTION_ID]) {
      const table = await within(list).findByRole('table', { name: new RegExp(id) });
      const scroller = table.parentElement;
      expect(scroller).toHaveAttribute('tabindex', '0');
      expect(scroller).toHaveAccessibleName(new RegExp(`Members of collection ${id.slice(0, 12)}`));
    }
  });

  it('explains why one recording cannot be planned and offers no Preview', async () => {
    const research = createFakeResearch();
    serveHome(research, collectionPageJson([collectionJson(ONE_RECORDING_COLLECTION_ID)]));
    renderWithServices(<WalkForward planId={null} studyId={null} />, { research });

    const form = await screen.findByRole('region', { name: 'Plan a walk-forward study' });
    expect(await within(form).findByText('Not enough independent recordings for a walk-forward study')).toBeInTheDocument();
    expect(within(form).getByText(/holds 1 recording of AAPL/)).toBeInTheDocument();
    expect(within(form).getByText(/needs at least 2 independent recordings of AAPL/)).toBeInTheDocument();
    expect(within(form).getByText(/never splits, clones, or shifts a recording/)).toBeInTheDocument();
    expect(within(form).getByRole('button', { name: 'Preview plan' })).toBeDisabled();
  });

  it('previews exactly the request shown and links to its frozen plan', async () => {
    const research = createFakeResearch();
    serveHome(research);
    research.on('POST', `${RESEARCH_API}/walk-forward/plans`, {
      status: 202,
      body: { deduplicated: false, job: planJobJson({ state: 'queued', result: null, finished_at: null }) },
    });
    research.on('GET', `${RESEARCH_API}/jobs/${PLAN_JOB_ID}`, { status: 200, body: { job: planJobJson() } });
    const { user } = renderWithServices(<WalkForward planId={null} studyId={null} />, { research });

    const form = await screen.findByRole('region', { name: 'Plan a walk-forward study' });
    expect(await within(form).findByText(/This collection is generated test data/)).toBeInTheDocument();
    await user.clear(within(form).getByLabelText('Stress latency (ns)'));
    await user.type(within(form).getByLabelText('Stress latency (ns)'), '7000000');
    await user.click(within(form).getByRole('checkbox', { name: /Zero-fee reference/ }));
    await user.click(within(form).getByRole('button', { name: 'Preview plan' }));

    const previewed = within(form).getByLabelText('The previewed request');
    expect(within(previewed).getByText('0.05 USD per fill, 7000000 ns latency')).toBeInTheDocument();
    expect(within(previewed).getByText('0 USD per fill, 1000000 ns latency')).toBeInTheDocument();
    expect(await within(form).findByText('The plan is ready to review')).toBeInTheDocument();
    expect(within(form).getByRole('link', { name: 'Review the plan' })).toHaveAttribute(
      'href',
      `#/research/walk-forward/plans/${PLAN_ID}`,
    );
    expect(research.requestsTo('POST', `${RESEARCH_API}/walk-forward/plans`).map((request) => request.body)).toEqual([
      {
        collection_id: WEEK_COLLECTION_ID,
        instrument: 'AAPL',
        minimum_training_sources: '1',
        configuration: DEFAULT_CONFIGURATION,
        scenarios: [
          { name: 'base', fee_per_fill: '0.01', latency_ns: '1000000' },
          { name: 'stress', fee_per_fill: '0.05', latency_ns: '7000000' },
          { name: 'zero_fee', fee_per_fill: '0', latency_ns: '1000000' },
        ],
      },
    ]);
    await user.click(within(form).getByRole('button', { name: 'Edit' }));
    expect(within(form).getByRole('button', { name: 'Preview plan' })).toBeEnabled();
  });

  it('shows a refused preview with its reason', async () => {
    const research = createFakeResearch();
    serveHome(research);
    research.on('POST', `${RESEARCH_API}/walk-forward/plans`, {
      status: 202,
      body: { deduplicated: false, job: { ...failedPlanJobJson(), state: 'queued', error: null, finished_at: null } },
    });
    research.on('GET', `${RESEARCH_API}/jobs/${PLAN_JOB_ID}`, { status: 200, body: { job: failedPlanJobJson() } });
    const { user } = renderWithServices(<WalkForward planId={null} studyId={null} />, { research });

    const form = await screen.findByRole('region', { name: 'Plan a walk-forward study' });
    await user.click(await within(form).findByRole('button', { name: 'Preview plan' }));
    expect(await within(form).findByText('Preview failed')).toBeInTheDocument();
    expect(within(form).getByText(/Not enough independent recordings: 1 recording\(s\) of collection/)).toBeInTheDocument();
    expect(within(form).getByText('insufficient_coverage')).toBeInTheDocument();
  });

  it('reviews a frozen plan and starts it by its ID alone, following its progress', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/walk-forward/plans/${PLAN_ID}`, { status: 200, body: planJson() });
    const running = studyJobJson({
      state: 'running',
      result: null,
      finished_at: null,
      progress: { fold: '2', folds: '2', model: 'logistic', scenario: 'stress', stage: 'fold', step: 'run' },
    });
    research.on('POST', `${RESEARCH_API}/walk-forward/studies`, { status: 202, body: { deduplicated: false, job: running } });
    let polls = 0;
    research.on('GET', `${RESEARCH_API}/jobs/${STUDY_JOB_ID}`, () => {
      polls += 1;
      return { status: 200, body: { job: polls === 1 ? running : studyJobJson() } };
    });
    const { user } = renderWithServices(<WalkForward planId={PLAN_ID} studyId={null} />, { research });

    expect(await screen.findByText('Synthetic recordings', { selector: 'p' })).toBeInTheDocument();
    const folds = screen.getByRole('table', { name: /training recordings, cutoff, and evaluation recording/ });
    const [, first] = within(folds).getAllByRole('row');
    if (first === undefined) throw new Error('the plan has no fold row');
    const cell = (label: string) => first.querySelector(`td[data-label="${label}"]`);
    expect(cell('Class support down · flat · up')).toHaveTextContent('124 · 78 · 214');
    expect(cell('Trains on')).toHaveTextContent('synthetic-2026-09-21: 416 of 420 rows');
    expect(cell('Training rows')).toHaveTextContent('416 eligible');
    expect(cell('Training rows')).toHaveTextContent('4 excluded in synthetic-2026-09-21: label unavailable: no future observation');
    expect(cell('Evaluates')).toHaveTextContent('synthetic-2026-09-22: 360 rows, the whole recording');
    expect(cell('Information boundary')).toHaveTextContent('2026-09-2213:30:00.000264444exclusive');
    const scenarios = screen.getByRole('table', { name: 'The frozen fee and latency scenarios' });
    expect(within(scenarios).getByRole('cell', { name: /^Stress/ })).toBeInTheDocument();
    expect(within(scenarios).getByText('200000')).toBeInTheDocument();

    const start = screen.getByRole('region', { name: 'Start the study' });
    await user.click(within(start).getByRole('button', { name: 'Start study' }));
    expect(
      await within(start).findByText('Fold 2 of 2: running the engine: Logistic regression, Stress scenario'),
    ).toBeInTheDocument();
    expect(await within(start).findByText('Study published', {}, { timeout: 4000 })).toBeInTheDocument();
    expect(within(start).getByRole('link', { name: 'Open the study' })).toHaveAttribute(
      'href',
      `#/research/walk-forward/studies/${COMPLETE_STUDY_ID}`,
    );
    expect(research.requestsTo('POST', `${RESEARCH_API}/walk-forward/studies`).map((request) => request.body)).toEqual([
      { plan_id: PLAN_ID },
    ]);
  });

  it('shows a partial study plainly, trading first, prediction quality apart, nothing missing as zero', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/walk-forward/studies/${PARTIAL_STUDY_ID}`, {
      status: 200,
      body: partialStudyDetailJson(),
    });
    renderWithServices(<WalkForward planId={null} studyId={PARTIAL_STUDY_ID} />, { research });

    const partial = await screen.findByText('This study is partial');
    expect(partial.parentElement).toHaveTextContent(
      'Fold 1, Logistic regression: Too few training rows of some class to fit',
    );
    expect(screen.getByText('Synthetic recordings', { selector: 'p' })).toBeInTheDocument();

    const trading = screen.getByRole('table', { name: /one row per engine run/ });
    const rows = within(trading).getAllByRole('row').slice(1);
    expect(rows).toHaveLength(8);
    // Each phone card names its fold, model, and scenario, and every figure.
    for (const row of rows) {
      for (const cell of within(row).getAllByRole('cell')) {
        expect(cell).toHaveAttribute('data-label');
      }
    }
    const unfit = rows.filter((row) => within(row).queryByText('Not run') !== null);
    expect(unfit).toHaveLength(2);
    for (const row of unfit) {
      expect(within(row).getByRole('cell', { name: /^Fold 1 · synthetic-2026-09-21/ })).toBeInTheDocument();
      expect(within(row).getByText('Logistic regression')).toBeInTheDocument();
      expect(within(row).getAllByText('Not available').length).toBeGreaterThan(5);
      expect(within(row).queryByText('0')).toBeNull();
    }
    expect(within(trading).getAllByRole('link', { name: /^Open run/ })).toHaveLength(6);

    const totals = screen.getByRole('table', { name: /summed over its folds' independent accounts/ });
    expect(within(totals).getAllByText('Complete total unavailable')).toHaveLength(2);
    expect(within(totals).getAllByText('Partial: 1 of 2 folds (fold 2)')).toHaveLength(2);
    expect(within(totals).getAllByText('Complete: all 2 folds')).toHaveLength(2);

    const quality = screen.getByRole('table', { name: /classification quality, then each model's scores pooled/ });
    expect(within(quality).getByText(/Unevaluable: Too few training rows/)).toBeInTheDocument();
    // Confusion counts and every class's recall, per fold and pooled.
    const pooledLogistic = within(quality)
      .getAllByRole('row')
      .find((row) => within(row).queryByText('Pooled over 1 of 2 folds (fold 2)') !== null);
    expect(pooledLogistic).toBeDefined();
    const confusion = pooledLogistic?.querySelector('td[data-label^="Confusion"]');
    expect(confusion).toHaveTextContent(/down → \d+ · \d+ · \d+flat → \d+ · \d+ · \d+up → \d+ · \d+ · \d+/);
    expect(pooledLogistic?.querySelector('td[data-label="Rows predicted"]')).toHaveTextContent('Counted per fold');
    expect(pooledLogistic?.querySelector('td[data-label="Recall down · flat · up"]')).toHaveTextContent(
      /^\d+\.\d{2}%\d+\.\d{2}%\d+\.\d{2}%$/,
    );
    expect(within(quality).getByText('Pooled over 1 of 2 folds (fold 2)')).toBeInTheDocument();
    expect(within(quality).getByText('All folds, pooled')).toBeInTheDocument();
    expect(screen.getByText(/fold accuracies are never averaged/)).toBeInTheDocument();
    expect(screen.getByText(/Classification quality is not trading performance/)).toBeInTheDocument();
  });

  it('says which runs no longer verify and labels every figure that rests on them', async () => {
    const detail = partialStudyDetailJson();
    const artifacts = detail.artifacts as Json[];
    const [, second] = artifacts;
    if (second === undefined) throw new Error('the fixture has two folds');
    const foldTwo = second.models as Record<string, Json>;
    (foldTwo.logistic as { runs: Record<string, string> }).runs.base =
      'invalid: journal.ndjson differs from its manifest’s checksum';
    delete (foldTwo.majority as { runs: Record<string, string> }).runs.stress;
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/walk-forward/studies/${PARTIAL_STUDY_ID}`, { status: 200, body: detail });
    renderWithServices(<WalkForward planId={null} studyId={PARTIAL_STUDY_ID} />, { research });

    const warning = await screen.findByText('Some of this study’s artifacts do not verify');
    expect(warning.parentElement).toHaveTextContent(
      'Fold 2, Logistic regression, Base scenario run: journal.ndjson differs from its manifest’s checksum',
    );
    expect(warning.parentElement).toHaveTextContent('Fold 2, Majority class (no skill), Stress scenario run: not checked');
    const summary = screen.getByRole('region', { name: /AAPL · 2 folds/ });
    expect(within(summary).getByText('Unverified')).toBeInTheDocument();

    const trading = screen.getByRole('table', { name: /one row per engine run/ });
    const row = (fold: string, model: string, scenario: string) => {
      const found = within(trading)
        .getAllByRole('row')
        .find(
          (candidate) =>
            (candidate.querySelector('td[data-label="Fold"]')?.textContent ?? '').startsWith(fold) &&
            candidate.querySelector('td[data-label="Model"]')?.textContent === model &&
            (candidate.querySelector('td[data-label="Scenario"]')?.textContent ?? '').startsWith(scenario),
        );
      if (found === undefined) throw new Error(`no row for ${fold} ${model} ${scenario}`);
      return found;
    };
    const verification = (found: HTMLElement) => found.querySelector('td[data-label="Verification"]');
    expect(verification(row('Fold 2', 'Logistic regression', 'Base'))).toHaveTextContent(
      'Does not verifyjournal.ndjson differs from its manifest’s checksum',
    );
    expect(verification(row('Fold 2', 'Majority class (no skill)', 'Stress'))).toHaveTextContent('Not checked');
    expect(verification(row('Fold 2', 'Logistic regression', 'Stress'))).toHaveTextContent('Verifies');
    expect(verification(row('Fold 1', 'Logistic regression', 'Base'))).toHaveTextContent('No run');
    expect(row('Fold 2', 'Logistic regression', 'Base').querySelector('td[data-label="Run"]')).toHaveTextContent(
      'Completed, unverified',
    );

    // Totals that include an unverified run say so; the rest do not.
    const totals = screen.getByRole('table', { name: /summed over its folds' independent accounts/ });
    const coverage = within(totals)
      .getAllByRole('row')
      .slice(1)
      .map((candidate) => candidate.querySelector('td[data-label="Coverage"]')?.textContent ?? '');
    expect(coverage.filter((text) => text.includes('Unverified'))).toHaveLength(2);
    expect(coverage.filter((text) => text.includes('includes fold 2’s run, which does not verify'))).toHaveLength(1);
  });

  it('opens a fold run like any result, with its fold provenance and no split', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/results/${FOLD_RUN_ID}`, { status: 200, body: foldResultJson() });
    research.on('GET', `${RESEARCH_API}/results/${FOLD_RUN_ID}/equity`, {
      status: 200,
      body: { points: [], next_cursor: null },
    });
    research.on('GET', `${RESEARCH_API}/results/${FOLD_RUN_ID}/trades`, {
      status: 200,
      body: { trades: [], next_cursor: null },
    });
    research.on('GET', `${RESEARCH_API}/results/${FOLD_RUN_ID}/decisions`, {
      status: 200,
      body: { decisions: [], next_cursor: null },
    });
    renderWithServices(<ResultView runId={FOLD_RUN_ID} />, { research });

    expect(await screen.findByText(/AAPL · Fold 2 window · Logistic regression, fold 2 model/)).toBeInTheDocument();
    const provenance = screen.getByRole('region', { name: 'Assumptions and provenance' });
    expect(within(provenance).getByRole('link', { name: PLAN_ID })).toHaveAttribute(
      'href',
      `#/research/walk-forward/plans/${PLAN_ID}`,
    );
    expect(within(provenance).getByText('Information boundary (exclusive)')).toBeInTheDocument();
    expect(within(provenance).queryByText('Split')).toBeNull();
    expect(within(provenance).queryByText('Labels')).toBeNull();
  });

  it('says plainly when the research service cannot be reached', async () => {
    const research = createFakeResearch();
    research.failNext('GET', `${RESEARCH_API}/status`);
    renderWithServices(<WalkForward planId={null} studyId={null} />, { research });
    expect(await screen.findByText('The research service is not reachable')).toBeInTheDocument();
  });
});
