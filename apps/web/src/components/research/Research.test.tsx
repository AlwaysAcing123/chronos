import { screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { RESEARCH_API } from '../../api/research';
import type { FakeResearch } from '../../test/fakeResearch';
import { createFakeResearch } from '../../test/fakeResearch';
import { errorJson } from '../../test/fixtures';
import { renderWithServices } from '../../test/renderWithServices';
import {
  DEFAULT_CONFIGURATION,
  EXPERIMENT_ID,
  RUN_ID,
  decisionsJson,
  equityPointsJson,
  experimentPageJson,
  jobJson,
  researchStatusJson,
  resultJson,
  resultListingJson,
  tradesJson,
} from '../../test/researchFixtures';
import { Research } from './Research';

function serveHome(research: FakeResearch) {
  research.on('GET', `${RESEARCH_API}/status`, { status: 200, body: researchStatusJson() });
  research.on('GET', `${RESEARCH_API}/experiments`, { status: 200, body: experimentPageJson() });
  research.on('GET', `${RESEARCH_API}/runs`, { status: 200, body: { runs: [], next_cursor: null } });
  research.on('GET', `${RESEARCH_API}/results`, {
    status: 200,
    body: { results: [resultListingJson()], invalid_entries: [], next_cursor: null },
  });
}

function serveResult(research: FakeResearch, result = resultJson()) {
  research.on('GET', `${RESEARCH_API}/results/${RUN_ID}`, { status: 200, body: { result } });
  research.on('GET', `${RESEARCH_API}/results/${RUN_ID}/equity`, {
    status: 200,
    body: { points: equityPointsJson(), next_cursor: null },
  });
  const [first, second, third] = tradesJson();
  research.on('GET', `${RESEARCH_API}/results/${RUN_ID}/trades`, (request) =>
    request.query.get('cursor') === null
      ? { status: 200, body: { trades: [first, second], next_cursor: 'next' } }
      : { status: 200, body: { trades: [third], next_cursor: null } },
  );
  research.on('GET', `${RESEARCH_API}/results/${RUN_ID}/decisions`, {
    status: 200,
    body: { decisions: decisionsJson(), next_cursor: null },
  });
}

describe('Research', () => {
  it('starts an evaluation only after its assumptions are reviewed', async () => {
    const research = createFakeResearch();
    serveHome(research);
    research.on('POST', `${RESEARCH_API}/runs`, { status: 202, body: { deduplicated: false, job: jobJson() } });
    const { user } = renderWithServices(<Research resultId={null} />, { research });

    const model = await screen.findByRole('region', { name: 'Model' });
    // The fitted instrument is chosen first; the unevaluable one says why.
    expect(within(model).getByRole('radio', { name: /SPY/ })).toBeChecked();
    expect(within(model).getByText('Logistic unevaluable: insufficient class support')).toBeInTheDocument();
    // Prediction metrics are labelled as classification, not profit.
    expect(within(model).getByText(/Accuracy is not profit/)).toBeInTheDocument();

    const form = screen.getByRole('region', { name: 'Evaluation' });
    expect(within(form).getByLabelText('Fee per fill (USD)')).toHaveValue('0.01');
    await user.click(within(form).getByRole('button', { name: 'Review evaluation' }));
    expect(research.requestsTo('POST', `${RESEARCH_API}/runs`)).toHaveLength(0);
    expect(within(form).getByText(/never short/)).toBeInTheDocument();
    await user.click(within(form).getByRole('button', { name: 'Start evaluation' }));

    expect(await within(form).findByText('Evaluation queued')).toBeInTheDocument();
    expect(research.requestsTo('POST', `${RESEARCH_API}/runs`)[0]?.body).toEqual({
      configuration: DEFAULT_CONFIGURATION,
      experiment_id: EXPERIMENT_ID,
      instrument: 'SPY',
      model: 'logistic',
      window: 'validation',
    });
  });

  it('does not offer an unevaluable model and names a refused field', async () => {
    const research = createFakeResearch();
    serveHome(research);
    research.on('POST', `${RESEARCH_API}/runs`, {
      status: 422,
      body: errorJson('invalid_configuration', 'configuration.fee_per_fill must be nonnegative', {
        field: 'configuration.fee_per_fill',
        reason: 'out_of_domain',
      }),
    });
    const { user } = renderWithServices(<Research resultId={null} />, { research });
    const model = await screen.findByRole('region', { name: 'Model' });
    await user.click(within(model).getByRole('radio', { name: /AAPL/ }));
    const form = screen.getByRole('region', { name: 'Evaluation' });
    expect(within(form).getByRole('option', { name: /Logistic regression \(unevaluable\)/ })).toBeDisabled();
    expect(within(form).getByRole('button', { name: 'Review evaluation' })).toBeDisabled();

    await user.click(within(model).getByRole('radio', { name: /SPY/ }));
    await user.clear(within(form).getByLabelText('Fee per fill (USD)'));
    await user.type(within(form).getByLabelText('Fee per fill (USD)'), '-0.01');
    await user.click(within(form).getByRole('button', { name: 'Review evaluation' }));
    await user.click(within(form).getByRole('button', { name: 'Start evaluation' }));
    expect(await within(form).findByText('The evaluation was not started')).toBeInTheDocument();
    expect(within(form).getByText('out_of_domain')).toBeInTheDocument();
    await user.click(within(form).getByRole('button', { name: 'Edit' }));
    expect(within(form).getByLabelText('Fee per fill (USD)')).toHaveAttribute('aria-invalid', 'true');
  });

  it('submits the reviewed instrument even if the catalog selection changes', async () => {
    const research = createFakeResearch();
    serveHome(research);
    research.on('POST', `${RESEARCH_API}/runs`, { status: 202, body: { deduplicated: false, job: jobJson() } });
    const { user } = renderWithServices(<Research resultId={null} />, { research });
    const catalog = await screen.findByRole('region', { name: 'Model' });
    const form = screen.getByRole('region', { name: 'Evaluation' });
    await user.click(within(form).getByRole('button', { name: 'Review evaluation' }));
    await user.click(within(catalog).getByRole('radio', { name: /AAPL/ }));
    const review = within(form).getByLabelText('Review the evaluation');
    expect(within(review).getByText('SPY')).toBeInTheDocument();
    await user.click(within(form).getByRole('button', { name: 'Start evaluation' }));
    await within(form).findByText('Evaluation queued');
    expect(research.requestsTo('POST', `${RESEARCH_API}/runs`)[0]?.body).toMatchObject({ instrument: 'SPY' });
  });

  it('explains how to start a research service that is not reachable', async () => {
    const research = createFakeResearch();
    research.failNext('GET', `${RESEARCH_API}/status`);
    renderWithServices(<Research resultId={null} />, { research });
    expect(await screen.findByText('The research service is not reachable')).toBeInTheDocument();
    expect(screen.getByText(/python -m chronos_research serve/)).toBeInTheDocument();
  });

  it('says when research is not configured at all', () => {
    renderWithServices(<Research resultId={null} />);
    expect(screen.getByText('Research is not configured')).toBeInTheDocument();
  });

  it('never presents a simulation as a live deployment', () => {
    renderWithServices(<Research resultId={null} />);
    expect(screen.getByText(/Every evaluation here is a historical simulation/)).toHaveTextContent(
      'it never touches a paper account',
    );
    expect(screen.getByText(/Every evaluation here is a historical simulation/)).toHaveTextContent(
      'only after you start it',
    );
  });

  it('shows a failed and an interrupted run with their reasons', async () => {
    const research = createFakeResearch();
    serveHome(research);
    research.on('GET', `${RESEARCH_API}/runs`, {
      status: 200,
      body: {
        runs: [
          jobJson({ job_id: '1'.repeat(32), state: 'failed', error: { code: 'evaluation_failed', message: 'the split was refused' } }),
          jobJson({ job_id: '2'.repeat(32), state: 'interrupted', error: { code: 'interrupted', message: 'the service stopped' } }),
        ],
        next_cursor: null,
      },
    });
    renderWithServices(<Research resultId={null} />, { research });
    const runs = await screen.findByRole('region', { name: 'Runs' });
    expect(await within(runs).findByText('Failed')).toBeInTheDocument();
    expect(within(runs).getByText('the split was refused')).toBeInTheDocument();
    expect(within(runs).getByText('Interrupted')).toBeInTheDocument();
  });

  it('shows a result whose unavailable values are never zero, with an exact chart readout', async () => {
    const research = createFakeResearch();
    serveResult(research);
    const { user } = renderWithServices(<Research resultId={RUN_ID} />, { research });

    const outcome = await screen.findByRole('region', { name: 'Outcome' });
    const equityRow = within(outcome).getByRole('row', { name: /^Final equity/ });
    const [strategy, buyAndHold, cash] = within(equityRow).getAllByRole('cell');
    expect(strategy).toHaveTextContent('10000.13');
    expect(buyAndHold).toHaveTextContent('Unavailable: stale market state');
    expect(buyAndHold).not.toHaveTextContent(/^0$/);
    expect(cash).toHaveTextContent('10000');

    const slider = await screen.findByRole('slider', { name: /Equity over the window/ });
    expect(slider).toHaveAttribute('aria-valuenow', '4');
    expect(slider.getAttribute('aria-valuetext')).toContain('Buy and hold unavailable (position_mark_unavailable)');
    slider.focus();
    await user.keyboard('{ArrowLeft}');
    expect(slider).toHaveAttribute('aria-valuenow', '3');
    expect(screen.getByText(/Point 3 of 4/)).toBeInTheDocument();
    expect(slider.getAttribute('aria-valuetext')).toContain('Strategy 9999.39');
    await user.keyboard('{Home}');
    expect(slider).toHaveAttribute('aria-valuenow', '1');

    const trades = screen.getByRole('region', { name: 'Trades' });
    expect(await within(trades).findAllByText('Filled')).toHaveLength(2);
    await user.click(within(trades).getByRole('button', { name: 'Load more' }));
    expect(await within(trades).findByText('Expired at window end')).toBeInTheDocument();
    expect(within(trades).getByText('Never executed')).toBeInTheDocument();

    const decisions = screen.getByRole('region', { name: 'Decisions' });
    expect(await within(decisions).findByText('50.1539269%')).toBeInTheDocument();
    const provenance = screen.getByRole('region', { name: 'Assumptions and provenance' });
    expect(within(provenance).getByText(RUN_ID)).toBeInTheDocument();
  });

  it('says a corrupt result cannot be shown', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/results/${RUN_ID}`, {
      status: 422,
      body: errorJson('artifact_invalid', 'the result cannot be read: journal.ndjson differs from its manifest’s checksum'),
    });
    renderWithServices(<Research resultId={RUN_ID} />, { research });
    expect(await screen.findByText('This result cannot be shown')).toBeInTheDocument();
    await waitFor(() => {
      expect(screen.getByText(/differs from its manifest/)).toBeInTheDocument();
    });
  });
});
