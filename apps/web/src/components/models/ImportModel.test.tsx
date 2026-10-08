import { screen, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { RESEARCH_API } from '../../api/research';
import { deploymentJson } from '../../test/deploymentFixtures';
import { createFakeApi } from '../../test/fakeApi';
import { createFakeResearch } from '../../test/fakeResearch';
import type { FakeResearch } from '../../test/fakeResearch';
import { errorJson } from '../../test/fixtures';
import {
  IMPORTED_MODEL_SHA256,
  IMPORTED_VERSION_ID,
  IMPORT_ID,
  IMPORT_JOB_ID,
  IMPORT_MANIFEST_TEXT,
  importJobJson,
  importedDetailJson,
  stagedImportJson,
} from '../../test/importFixtures';
import { renderWithServices } from '../../test/renderWithServices';
import {
  SANDBOX_UNAVAILABLE,
  experimentPageJson,
  importsJson,
  researchStatusJson,
} from '../../test/researchFixtures';
import { Models } from './Models';

const MODEL_BYTES = new Uint8Array([8, 8, 18, 4, 116, 101, 115, 116]);

async function hex(bytes: Uint8Array<ArrayBuffer>): Promise<string> {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return Array.from(digest, (byte) => byte.toString(16).padStart(2, '0')).join('');
}

function serve(research: FakeResearch, sandbox?: Record<string, unknown>) {
  research.on('GET', `${RESEARCH_API}/status`, {
    status: 200,
    body: researchStatusJson(sandbox === undefined ? {} : { imports: importsJson(sandbox) }),
  });
  research.on('GET', `${RESEARCH_API}/experiments`, { status: 200, body: experimentPageJson() });
  research.on('GET', `${RESEARCH_API}/models`, {
    status: 200,
    body: { models: [], invalid_entries: [], next_cursor: null },
  });
}

async function chooseFiles(user: ReturnType<typeof renderWithServices>['user'], manifest = IMPORT_MANIFEST_TEXT) {
  const form = await screen.findByRole('region', { name: 'Import an ONNX model' });
  await user.upload(
    within(form).getByLabelText('Import manifest (JSON)'),
    new File([manifest], 'import-manifest.json', { type: 'application/json' }),
  );
  await user.upload(within(form).getByLabelText('Model (.onnx)'), new File([MODEL_BYTES], 'model.onnx'));
  await user.click(within(form).getByRole('button', { name: 'Review import' }));
  return form;
}

describe('ONNX imports', () => {
  it('sends the reviewed manifest text and model bytes only after review, and follows the job', async () => {
    const digest = await hex(MODEL_BYTES);
    const research = createFakeResearch();
    serve(research);
    research.on('POST', `${RESEARCH_API}/imports`, { status: 201, body: stagedImportJson() });
    research.on('PUT', `${RESEARCH_API}/imports/${IMPORT_ID}/model`, {
      status: 201,
      body: stagedImportJson({
        sha256: digest,
        size_bytes: String(MODEL_BYTES.length),
        uploaded_at: '2026-10-07T21:00:01.000000000Z',
      }),
    });
    research.on('POST', `${RESEARCH_API}/imports/${IMPORT_ID}/validation`, {
      status: 202,
      body: { deduplicated: false, job: importJobJson({ request: { ...importJobRequest(), model_sha256: digest } }) },
    });
    research.on('GET', `${RESEARCH_API}/jobs/${IMPORT_JOB_ID}`, {
      status: 200,
      body: {
        job: importJobJson({
          state: 'succeeded',
          request: { ...importJobRequest(), model_sha256: digest },
          started_at: '2026-10-07T21:00:03.000000000Z',
          finished_at: '2026-10-07T21:00:05.000000000Z',
          result: {
            model_version_id: IMPORTED_VERSION_ID,
            outcome: 'published',
            binding_outcome: 'published',
            binding_id: 'b'.repeat(64),
          },
        }),
      },
    });
    const { user } = renderWithServices(<Models modelId={null} />, { research });

    const form = await chooseFiles(user);
    expect(await within(form).findByText(digest)).toBeInTheDocument();
    expect(within(form).getByText('aapl-logistic-onnx')).toBeInTheDocument();
    expect(within(form).getByText('Declared by uploader, not verified')).toBeInTheDocument();
    expect(within(form).getByText('Declared by the uploader, not verified')).toBeInTheDocument();
    // Nothing is sent before the review is confirmed.
    expect(research.requestsTo('POST', `${RESEARCH_API}/imports`)).toHaveLength(0);

    await user.click(within(form).getByRole('button', { name: 'Validate and import' }));
    expect(await within(form).findByText('Model version imported')).toBeInTheDocument();
    expect(within(form).getByRole('link', { name: 'Open the model version' })).toHaveAttribute(
      'href',
      `#/models/${IMPORTED_VERSION_ID}`,
    );
    const [staged] = research.requestsTo('POST', `${RESEARCH_API}/imports`);
    expect(staged?.text).toBe(IMPORT_MANIFEST_TEXT);
    expect(staged?.contentType).toBe('application/json');
    const [uploaded] = research.requestsTo('PUT', `${RESEARCH_API}/imports/${IMPORT_ID}/model`);
    expect(uploaded?.contentType).toBe('application/octet-stream');
    expect(Array.from(uploaded?.bytes ?? [])).toEqual(Array.from(MODEL_BYTES));
    const [validation] = research.requestsTo('POST', `${RESEARCH_API}/imports/${IMPORT_ID}/validation`);
    expect(validation?.body).toEqual({ model_sha256: digest });
  });

  it('does not validate when the service received other bytes than were reviewed', async () => {
    const research = createFakeResearch();
    serve(research);
    research.on('POST', `${RESEARCH_API}/imports`, { status: 201, body: stagedImportJson() });
    research.on('PUT', `${RESEARCH_API}/imports/${IMPORT_ID}/model`, {
      status: 201,
      body: stagedImportJson({ sha256: 'f'.repeat(64), size_bytes: '8', uploaded_at: '2026-10-07T21:00:01.000000000Z' }),
    });
    const { user } = renderWithServices(<Models modelId={null} />, { research });

    const form = await chooseFiles(user);
    await user.click(await within(form).findByRole('button', { name: 'Validate and import' }));
    expect(await within(form).findByText('The model file was not accepted')).toBeInTheDocument();
    expect(research.requestsTo('POST', `${RESEARCH_API}/imports/${IMPORT_ID}/validation`)).toHaveLength(0);
  });

  it('names the step that refused an import and sends nothing after it', async () => {
    const research = createFakeResearch();
    serve(research);
    research.on('POST', `${RESEARCH_API}/imports`, {
      status: 422,
      body: errorJson('incompatible_contract', 'the inputs must be top_of_book_baseline version 1'),
    });
    const { user } = renderWithServices(<Models modelId={null} />, { research });

    const form = await chooseFiles(user);
    await user.click(await within(form).findByRole('button', { name: 'Validate and import' }));
    expect(await within(form).findByText('The manifest was not accepted')).toBeInTheDocument();
    expect(research.requestsTo('PUT', `${RESEARCH_API}/imports/${IMPORT_ID}/model`)).toHaveLength(0);
  });

  it('offers no validation without the sandbox, and says why', async () => {
    const research = createFakeResearch();
    serve(research, SANDBOX_UNAVAILABLE);
    const { user } = renderWithServices(<Models modelId={null} />, { research });

    const form = await chooseFiles(user);
    const alert = await within(form).findByText('The ONNX sandbox is unavailable');
    expect(alert).toBeInTheDocument();
    expect(within(form).getByText('image_not_built')).toBeInTheDocument();
    expect(within(form).getByRole('button', { name: 'Validate and import' })).toBeDisabled();
  });

  it('refuses to review a file that is not an import manifest', async () => {
    const research = createFakeResearch();
    serve(research);
    const { user } = renderWithServices(<Models modelId={null} />, { research });

    const form = await chooseFiles(user, '{"schema": {"name": "something else"}}');
    expect(await within(form).findByText('The files could not be reviewed')).toBeInTheDocument();
    expect(within(form).queryByRole('button', { name: 'Validate and import' })).not.toBeInTheDocument();
  });

  it("shows an import's declarations apart from what the sandbox validated", async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/models/${IMPORTED_VERSION_ID}`, {
      status: 200,
      body: { model: importedDetailJson() },
    });
    renderWithServices(<Models modelId={IMPORTED_VERSION_ID} />, { research });

    const provenance = await screen.findByRole('region', { name: 'Training provenance' });
    expect(within(provenance).getByText('Import (declared provenance)')).toBeInTheDocument();
    expect(within(provenance).getByText('Declared by the uploader, not verified')).toBeInTheDocument();
    expect(within(provenance).getByText('Declared by uploader, not verified')).toBeInTheDocument();
    expect(within(provenance).getByText('Sandbox validation')).toBeInTheDocument();
    expect(within(provenance).getByText('1.30.0')).toBeInTheDocument();
    expect(within(provenance).queryByText('Verified by Chronos')).not.toBeInTheDocument();
    const notVerified = within(provenance).getByRole('heading', { name: 'Not verified' }).parentElement;
    expect(notVerified).toHaveTextContent('Training.');
    expect(notVerified).toHaveTextContent('Information cutoff.');
    const identity = screen.getByRole('region', { name: 'aapl-logistic-onnx 1' });
    expect(within(identity).getByText('Imported ONNX classifier')).toBeInTheDocument();
    expect(identity).toHaveTextContent('never parses the model itself');
  });
});

function importJobRequest(): Record<string, unknown> {
  return importJobJson().request as Record<string, unknown>;
}

describe('deploying a registered version', () => {
  it('creates a stopped deployment of exactly the version, by its ID, after review', async () => {
    const api = createFakeApi();
    api.on('GET', '/api/v1/deployments', { status: 200, body: { enabled: true, deployments: [], next_cursor: null } });
    api.on('POST', '/api/v1/deployments', (request) => ({
      status: 201,
      body: {
        created: true,
        deployment: deploymentJson({
          deployment_id: (request.body as Record<string, unknown>).deployment_id,
          model: {
            reference: 'registered',
            model_version_id: IMPORTED_VERSION_ID,
            name: 'onnx',
            instrument: 'AAPL',
            parameters_sha256: IMPORTED_MODEL_SHA256,
            feature_set: 'top_of_book_baseline/1',
            training_cutoff: '1970-01-01T00:00:01.048000000Z',
            training_cutoff_basis: 'declared',
          },
        }),
      },
    }));
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/status`, { status: 200, body: researchStatusJson() });
    research.on('GET', `${RESEARCH_API}/models/${IMPORTED_VERSION_ID}`, {
      status: 200,
      body: { model: importedDetailJson() },
    });
    const { user } = renderWithServices(<Models modelId={IMPORTED_VERSION_ID} />, { api, research, deployments: true });

    const form = await screen.findByRole('region', { name: 'Deploy this version to paper' });
    expect(within(form).getByText('Its cutoff is declared, not verified')).toBeInTheDocument();
    await user.type(within(form).getByLabelText('Deployment ID'), 'onnx-aapl');
    await user.type(within(form).getByLabelText('New paper account ID'), 'model-onnx');
    await user.click(within(form).getByRole('button', { name: 'Review deployment' }));
    expect(within(form).getByText(IMPORTED_VERSION_ID)).toBeInTheDocument();
    expect(within(form).getByText(/never in the server/)).toBeInTheDocument();
    expect(api.requestsTo('POST', '/api/v1/deployments')).toHaveLength(0);
    await user.click(within(form).getByRole('button', { name: 'Create deployment' }));
    expect(await within(form).findByText('Deployment created, stopped')).toBeInTheDocument();
    const [sent] = api.requestsTo('POST', '/api/v1/deployments');
    expect(sent?.body).toMatchObject({
      deployment_id: 'onnx-aapl',
      account_id: 'model-onnx',
      instrument: 'AAPL',
      model_version_id: IMPORTED_VERSION_ID,
    });
    expect(sent?.body).not.toHaveProperty('experiment_id');
    expect(sent?.body).not.toHaveProperty('model');
  });
});
