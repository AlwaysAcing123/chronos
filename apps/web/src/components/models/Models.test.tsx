import { screen, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { RESEARCH_API } from '../../api/research';
import { createFakeResearch } from '../../test/fakeResearch';
import type { FakeResearch } from '../../test/fakeResearch';
import { errorJson } from '../../test/fixtures';
import {
  MODEL_VERSION_ID,
  REGISTRATION_JOB_ID,
  modelDetailJson,
  modelListingJson,
  registrationJobJson,
} from '../../test/modelFixtures';
import { renderWithServices } from '../../test/renderWithServices';
import { EXPERIMENT_ID, experimentPageJson, researchStatusJson } from '../../test/researchFixtures';
import { Models } from './Models';

function serveLibrary(research: FakeResearch, models = [modelListingJson()]) {
  research.on('GET', `${RESEARCH_API}/status`, {
    status: 200,
    body: researchStatusJson(),
  });
  research.on('GET', `${RESEARCH_API}/experiments`, {
    status: 200,
    body: experimentPageJson(),
  });
  research.on('GET', `${RESEARCH_API}/models`, (request) => ({
    status: 200,
    body: {
      models: request.query.get('state') === 'archived' ? [] : models,
      invalid_entries:
        request.query.get('state') === 'archived'
          ? []
          : [
              {
                model_version_id: 'f'.repeat(64),
                reason: 'parameters.json differs from the manifest',
              },
            ],
      next_cursor: null,
    },
  }));
}

describe('Models', () => {
  it('lists verified versions with their cutoff basis and reports those that do not verify', async () => {
    const research = createFakeResearch();
    serveLibrary(research);
    const { user } = renderWithServices(<Models modelId={null} />, {
      research,
    });

    const library = await screen.findByRole('region', {
      name: 'Model versions',
    });
    const link = await within(library).findByRole('link', {
      name: 'aapl-logistic-20260925 1',
    });
    expect(link).toHaveAttribute('href', `#/models/${MODEL_VERSION_ID}`);
    expect(within(library).getByText('Verified by Chronos')).toBeInTheDocument();
    expect(within(library).getByText('Verified experiment')).toBeInTheDocument();
    expect(within(library).getByText('Some model versions do not verify and cannot be used')).toBeInTheDocument();
    expect(research.requestsTo('GET', `${RESEARCH_API}/models`)[0]?.query.get('state')).toBe('active');

    await user.click(within(library).getByRole('radio', { name: 'Archived' }));
    expect(await within(library).findByText('No archived model versions.')).toBeInTheDocument();
  });

  it('registers a model only after its request is reviewed, and follows the job', async () => {
    const research = createFakeResearch();
    serveLibrary(research, []);
    research.on('POST', `${RESEARCH_API}/models`, {
      status: 202,
      body: { deduplicated: false, job: registrationJobJson() },
    });
    let reads = 0;
    research.on('GET', `${RESEARCH_API}/jobs/${REGISTRATION_JOB_ID}`, () => {
      reads += 1;
      return {
        status: 200,
        body: {
          job:
            reads === 1
              ? registrationJobJson({
                  state: 'running',
                  started_at: '2026-10-07T20:00:01.000000000Z',
                })
              : registrationJobJson({
                  state: 'succeeded',
                  started_at: '2026-10-07T20:00:01.000000000Z',
                  finished_at: '2026-10-07T20:00:05.000000000Z',
                  result: {
                    model_version_id: MODEL_VERSION_ID,
                    outcome: 'published',
                    binding_outcome: 'published',
                    binding_id: 'b'.repeat(64),
                  },
                }),
        },
      };
    });
    const { user } = renderWithServices(<Models modelId={null} />, {
      research,
    });

    const form = await screen.findByRole('region', {
      name: 'Register a model version',
    });
    await user.selectOptions(within(form).getByLabelText('Instrument'), 'SPY');
    // An unevaluable model cannot be chosen.
    await user.selectOptions(within(form).getByLabelText('Instrument'), 'AAPL');
    expect(
      within(form).getByRole('option', {
        name: /Logistic regression \(unevaluable\)/,
      }),
    ).toBeDisabled();
    await user.selectOptions(within(form).getByLabelText('Instrument'), 'SPY');
    expect(within(form).getByLabelText('Name')).toHaveAttribute('placeholder', 'spy-logistic');
    await user.clear(within(form).getByLabelText('Version'));
    await user.type(within(form).getByLabelText('Version'), '01');
    expect(within(form).getByRole('button', { name: 'Review registration' })).toBeDisabled();
    await user.clear(within(form).getByLabelText('Version'));
    await user.type(within(form).getByLabelText('Version'), '1');
    await user.click(within(form).getByRole('button', { name: 'Review registration' }));
    expect(research.requestsTo('POST', `${RESEARCH_API}/models`)).toHaveLength(0);
    expect(within(form).getByText(/cannot later point to another model/)).toBeInTheDocument();

    await user.click(within(form).getByRole('button', { name: 'Register' }));
    expect(research.requestsTo('POST', `${RESEARCH_API}/models`)[0]?.body).toEqual({
      experiment_id: EXPERIMENT_ID,
      instrument: 'SPY',
      model: 'logistic',
      name: 'spy-logistic',
      version: '1',
    });
    const done = await within(form).findByText('Model version registered', {}, { timeout: 5000 });
    expect(done).toBeInTheDocument();
    expect(within(form).getByRole('link', { name: 'Open the model version' })).toHaveAttribute(
      'href',
      `#/models/${MODEL_VERSION_ID}`,
    );
  });

  it('shows the contract, the verified provenance, and what was not verified', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/models/${MODEL_VERSION_ID}`, {
      status: 200,
      body: { model: modelDetailJson() },
    });
    renderWithServices(<Models modelId={MODEL_VERSION_ID} />, { research });

    const identity = await screen.findByRole('region', {
      name: 'aapl-logistic-20260925 1',
    });
    expect(within(identity).getByText(MODEL_VERSION_ID)).toBeInTheDocument();
    expect(within(identity).getByText(/Artifact integrity:/)).toBeInTheDocument();

    const contract = screen.getByRole('region', { name: 'Inputs and outputs' });
    const rows = within(within(contract).getByRole('table')).getAllByRole('row');
    expect(rows.map((row) => within(row).queryAllByRole('cell')[0]?.textContent).slice(1)).toEqual([
      'spread',
      'imbalance',
      'microprice_minus_midpoint',
      'bid_size',
      'ask_size',
    ]);
    expect(within(contract).getByText('down, flat, up')).toBeInTheDocument();

    const provenance = screen.getByRole('region', {
      name: 'Training provenance',
    });
    expect(within(provenance).getByText('Verified by Chronos')).toBeInTheDocument();
    expect(within(provenance).getByText('Source experiment matches')).toBeInTheDocument();
    expect(within(provenance).getByText('Source code revision.')).toBeInTheDocument();
    // Statements in a fixed order: experiment, sources, payload, cutoff.
    const statements = within(provenance)
      .getAllByRole('listitem')
      .flatMap((item) => item.querySelector('strong')?.textContent ?? []);
    expect(statements.slice(0, 4)).toEqual([
      'Source experiment.',
      'Source data.',
      'Parameters.',
      'Information cutoff.',
    ]);

    const uses = screen.getByRole('region', {
      name: 'Inference compatibility',
    });
    const deployment = within(uses).getByRole('row', { name: /Paper deployment/ });
    expect(within(deployment).getByText('Compatible')).toBeInTheDocument();
    expect(deployment).toHaveTextContent('only after an explicit Start');
    expect(
      within(within(uses).getByRole('row', { name: /Historical evaluation/ })).getByText('Compatible'),
    ).toBeInTheDocument();
  });

  it('archives a version only with a reason, and never changes anything else', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/models/${MODEL_VERSION_ID}`, {
      status: 200,
      body: { model: modelDetailJson() },
    });
    research.on('POST', `${RESEARCH_API}/models/${MODEL_VERSION_ID}/lifecycle`, {
      status: 200,
      body: {
        model_version_id: MODEL_VERSION_ID,
        lifecycle: {
          state: 'archived',
          reason: 'superseded',
          changed_at: '2026-10-07T20:00:00.000000000Z',
        },
      },
    });
    const { user } = renderWithServices(<Models modelId={MODEL_VERSION_ID} />, {
      research,
    });

    const identity = await screen.findByRole('region', {
      name: 'aapl-logistic-20260925 1',
    });
    const archive = within(identity).getByRole('button', {
      name: 'Archive this version',
    });
    expect(archive).toBeDisabled();
    await user.type(within(identity).getByLabelText('Reason, for the record'), 'superseded');
    await user.click(archive);
    expect(research.requestsTo('POST', `${RESEARCH_API}/models/${MODEL_VERSION_ID}/lifecycle`)[0]?.body).toEqual({
      reason: 'superseded',
      state: 'archived',
    });
  });

  it('reports a version that does not verify instead of showing it', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/models/${MODEL_VERSION_ID}`, {
      status: 422,
      body: errorJson('artifact_invalid', 'the model version cannot be read: parameters.json differs'),
    });
    renderWithServices(<Models modelId={MODEL_VERSION_ID} />, { research });
    expect(await screen.findByText('The model version could not be read')).toBeInTheDocument();
    expect(screen.queryByRole('region', { name: 'Training provenance' })).not.toBeInTheDocument();
  });
});
