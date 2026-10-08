import { screen, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { RESEARCH_API } from '../../api/research';
import {
  COMPARISON_ID,
  COMPARISON_JOB_ID,
  comparisonDetailJson,
  comparisonJobJson,
  comparisonPageJson,
} from '../../test/comparisonFixtures';
import { createFakeResearch } from '../../test/fakeResearch';
import type { FakeResearch } from '../../test/fakeResearch';
import { IMPORTED_VERSION_ID, importedListingJson } from '../../test/importFixtures';
import { MODEL_VERSION_ID, modelListingJson } from '../../test/modelFixtures';
import { renderWithServices } from '../../test/renderWithServices';
import { DEFAULT_CONFIGURATION, experimentPageJson, researchStatusJson } from '../../test/researchFixtures';
import { Comparisons } from './Comparisons';

type Json = Record<string, unknown>;

function serveHome(research: FakeResearch) {
  research.on('GET', `${RESEARCH_API}/status`, { status: 200, body: researchStatusJson() });
  research.on('GET', `${RESEARCH_API}/experiments`, { status: 200, body: experimentPageJson() });
  research.on('GET', `${RESEARCH_API}/models`, {
    status: 200,
    body: { models: [modelListingJson(), importedListingJson()], invalid_entries: [], next_cursor: null },
  });
  research.on('GET', `${RESEARCH_API}/comparisons`, { status: 200, body: comparisonPageJson() });
}

function serveDetail(research: FakeResearch, detail: Json = comparisonDetailJson()) {
  research.on('GET', `${RESEARCH_API}/comparisons/${COMPARISON_ID}`, { status: 200, body: detail });
}

describe('Comparisons', () => {
  it('starts a comparison only after its frozen spec is reviewed, and follows the job', async () => {
    const research = createFakeResearch();
    serveHome(research);
    research.on('POST', `${RESEARCH_API}/comparisons`, {
      status: 202,
      body: { deduplicated: false, job: comparisonJobJson({ state: 'queued', result: null, finished_at: null }) },
    });
    research.on('GET', `${RESEARCH_API}/jobs/${COMPARISON_JOB_ID}`, { status: 200, body: { job: comparisonJobJson() } });
    const { user } = renderWithServices(<Comparisons comparisonId={null} />, { research });

    const form = await screen.findByRole('region', { name: 'Compare model versions' });
    const review = within(form).getByRole('button', { name: 'Review comparison' });
    await user.click(await within(form).findByRole('checkbox', { name: /aapl-logistic-20260925 1/ }));
    // One member is not a comparison.
    expect(review).toBeDisabled();
    await user.click(within(form).getByRole('checkbox', { name: /aapl-logistic-onnx 1/ }));
    expect(within(form).getByText('Declared by uploader, not verified')).toBeInTheDocument();
    await user.click(review);

    const reviewed = within(form).getByLabelText('Review the comparison');
    expect(within(reviewed).getByText(MODEL_VERSION_ID)).toBeInTheDocument();
    expect(within(reviewed).getByText(/No member is retrained/)).toBeInTheDocument();
    expect(research.requestsTo('POST', `${RESEARCH_API}/comparisons`)).toHaveLength(0);
    await user.click(within(form).getByRole('button', { name: 'Start comparison' }));
    expect(await within(form).findByText('Comparison published')).toBeInTheDocument();
    expect(within(form).getByRole('link', { name: 'Open the comparison' })).toHaveAttribute(
      'href',
      `#/comparisons/${COMPARISON_ID}`,
    );
    expect(research.requestsTo('POST', `${RESEARCH_API}/comparisons`)[0]?.body).toEqual({
      model_version_ids: [MODEL_VERSION_ID, IMPORTED_VERSION_ID],
      split_artifact_id: 'f'.repeat(64),
      instrument: 'AAPL',
      window: 'test',
      configuration: DEFAULT_CONFIGURATION,
    });
  });

  it('lists published comparisons with each member’s state', async () => {
    const research = createFakeResearch();
    serveHome(research);
    renderWithServices(<Comparisons comparisonId={null} />, { research });

    const list = await screen.findByRole('region', { name: 'Published comparisons' });
    expect(await within(list).findByRole('link', { name: COMPARISON_ID.slice(0, 12) + '…' })).toBeInTheDocument();
    expect(within(list).getByText('spy-logistic 1')).toBeInTheDocument();
    expect(within(list).getByText('Refused')).toBeInTheDocument();
  });

  it('shows members side by side, with refusals, declared cutoffs, and reproducibility', async () => {
    const research = createFakeResearch();
    serveDetail(research);
    renderWithServices(<Comparisons comparisonId={COMPARISON_ID} />, { research });

    const table = await screen.findByRole('table', { name: /strategy results/ });
    const headers = within(table).getAllByRole('columnheader');
    expect(headers.map((header) => header.textContent)).toEqual([
      'Figure',
      expect.stringContaining('aapl-logistic 1'),
      expect.stringContaining('aapl-logistic-onnx 1'),
      expect.stringContaining('spy-logistic 1'),
    ]);
    const pnl = within(table).getByRole('row', { name: /^Net PnL \(USD\)/ });
    expect(within(pnl).getAllByText('-0.04')).toHaveLength(2);
    // A refused member's figures say why, never zero.
    expect(within(pnl).getByText('Refused')).toBeInTheDocument();
    const reference = within(table).getByRole('row', { name: /^Net PnL minus the reference/ });
    expect(within(reference).getByText('The reference itself')).toBeInTheDocument();
    expect(within(reference).getByText('0')).toBeInTheDocument();

    // Prediction quality, apart from trading, against the window's labels.
    const quality = screen.getByRole('table', { name: /classification quality against realized labels/ });
    const accuracy = within(quality).getByRole('row', { name: /^Accuracy/ });
    expect(within(accuracy).getAllByText('32.91%')).toHaveLength(2);
    expect(within(accuracy).getAllByTitle('0.3291139240506329')).toHaveLength(2);
    expect(within(accuracy).getByText('Refused')).toBeInTheDocument();
    const logLoss = within(quality).getByRole('row', { name: /^Log loss/ });
    expect(within(logLoss).getAllByText('1.2251')).toHaveLength(2);
    expect(within(within(quality).getByRole('row', { name: /^Rows scored/ })).getAllByText('79 of 80')).toHaveLength(2);
    const notScored = within(quality).getByRole('row', { name: /^Rows not scored/ });
    expect(within(notScored).getAllByText('label unavailable: no future observation')).toHaveLength(2);
    expect(screen.getByRole('heading', { name: 'How prediction quality is scored' })).toBeInTheDocument();
    // On a phone, where the column headers are hidden, every cell names its member.
    for (const cell of [...within(table).getAllByRole('cell'), ...within(quality).getAllByRole('cell')]) {
      expect(cell).toHaveAttribute('data-label', expect.stringMatching(/^(aapl-logistic|aapl-logistic-onnx|spy-logistic) 1$/));
    }

    expect(screen.getByText('Some results rest on declared cutoffs')).toBeInTheDocument();
    const benchmarks = screen.getByRole('region', { name: 'Benchmarks' });
    expect(within(benchmarks).getByText('Identical for every member')).toBeInTheDocument();
    const members = screen.getByRole('region', { name: 'Members and reproducibility' });
    expect(within(members).getByText('Predicts another instrument', { exact: false })).toBeInTheDocument();
    expect(within(members).getByText('instrument_mismatch')).toBeInTheDocument();
    expect(within(members).getAllByText('Verifies')).toHaveLength(4);
    const runLink = within(members).getAllByRole('link')[0];
    expect(runLink).toHaveAttribute('href', expect.stringMatching(/^#\/research\/results\/[0-9a-f]{64}$/));
  });

  it('reports a failed member with its reason and an artifact that no longer verifies', async () => {
    const research = createFakeResearch();
    const detail = comparisonDetailJson();
    const comparison = detail.comparison as { members: Json[] };
    const onnx = comparison.members[1] ?? {};
    comparison.members[1] = {
      ...Object.fromEntries(Object.entries(onnx).filter(([key]) => !['accounts', 'deltas', 'predictive', 'run'].includes(key))),
      state: 'failed',
      reason: 'sandbox_timeout',
      detail: 'the sandbox did not reply in time',
    };
    (detail.artifacts as Json[])[0] = { ...(detail.artifacts as Json[])[0], run: 'invalid: journal.ndjson differs' };
    serveDetail(research, detail);
    renderWithServices(<Comparisons comparisonId={COMPARISON_ID} />, { research });

    const table = await screen.findByRole('table', { name: /strategy results/ });
    const pnl = within(table).getByRole('row', { name: /^Net PnL \(USD\)/ });
    expect(within(pnl).getByText('Failed')).toBeInTheDocument();
    expect(within(pnl).getAllByText('-0.04')).toHaveLength(1);
    const members = screen.getByRole('region', { name: 'Members and reproducibility' });
    expect(within(members).getByText('The ONNX sandbox failed', { exact: false })).toBeInTheDocument();
    expect(within(members).getByText('sandbox_timeout')).toBeInTheDocument();
    expect(within(members).getByText('Does not verify')).toBeInTheDocument();
    const quality = screen.getByRole('table', { name: /classification quality against realized labels/ });
    expect(within(within(quality).getByRole('row', { name: /^Accuracy/ })).getByText('Failed')).toBeInTheDocument();
  });

  it('shows a member that could not be scored, and an undefined metric, with their reasons', async () => {
    const research = createFakeResearch();
    const detail = comparisonDetailJson();
    const comparison = detail.comparison as { members: Json[] };
    const logistic = comparison.members[0] as { predictive: { metrics: Json } };
    logistic.predictive.metrics.log_loss = { undefined: 'zero_probability_for_a_true_class' };
    comparison.members[1] = {
      ...comparison.members[1],
      predictive: { unavailable: 'scoring_failed', detail: 'the schedule rows are not the window rows' },
    };
    serveDetail(research, detail);
    renderWithServices(<Comparisons comparisonId={COMPARISON_ID} />, { research });

    const quality = await screen.findByRole('table', { name: /classification quality against realized labels/ });
    const logLoss = within(quality).getByRole('row', { name: /^Log loss/ });
    expect(within(logLoss).getByText('Undefined: zero probability for a true class')).toBeInTheDocument();
    expect(within(logLoss).getByText('Not scored: scoring failed')).toBeInTheDocument();
    expect(within(logLoss).queryByText('0')).not.toBeInTheDocument();
    // Its trading results stand.
    const trading = screen.getByRole('table', { name: /strategy results/ });
    expect(within(within(trading).getByRole('row', { name: /^Net PnL \(USD\)/ })).getAllByText('-0.04')).toHaveLength(2);
  });
});
