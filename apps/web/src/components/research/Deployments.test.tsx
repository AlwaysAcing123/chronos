import { screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { App } from '../../App';
import { RESEARCH_API } from '../../api/research';
import { createFakeApi } from '../../test/fakeApi';
import type { FakeApi } from '../../test/fakeApi';
import { createFakeResearch } from '../../test/fakeResearch';
import type { Json } from '../../test/fixtures';
import { ACCOUNT_ID, accountJson } from '../../test/fixtures';
import {
  rememberSelectedAccount,
  renderWithServices,
  serveAccount,
} from '../../test/renderWithServices';
import { decisionJson, deploymentJson } from '../../test/deploymentFixtures';
import { EXPERIMENT_ID, experimentPageJson, researchStatusJson } from '../../test/researchFixtures';
import { ppbAsRatio, ratioAsPpb } from './DeploymentForm';
import { Research } from './Research';

function serveResearch() {
  const research = createFakeResearch();
  research.on('GET', `${RESEARCH_API}/status`, { status: 200, body: researchStatusJson() });
  research.on('GET', `${RESEARCH_API}/experiments`, { status: 200, body: experimentPageJson() });
  research.on('GET', `${RESEARCH_API}/runs`, { status: 200, body: { runs: [], next_cursor: null } });
  research.on('GET', `${RESEARCH_API}/results`, { status: 200, body: { results: [], invalid_entries: [], next_cursor: null } });
  return research;
}

function serveDeployments(api: FakeApi, deployments: Json[], enabled = true) {
  api.on('GET', '/api/v1/deployments', { status: 200, body: { enabled, deployments, next_cursor: null } });
}

describe('exact threshold conversions', () => {
  it('never goes through floating point', () => {
    expect(ratioAsPpb('0.55')).toBe('550000000');
    expect(ratioAsPpb('1')).toBe('1000000000');
    expect(ratioAsPpb('0.000000001')).toBe('1');
    expect(ratioAsPpb('1.5')).toBeNull();
    expect(ratioAsPpb('0.1234567891')).toBeNull();
    expect(ppbAsRatio('550000000')).toBe('0.55');
    expect(ppbAsRatio('1000000000')).toBe('1');
  });
});

describe('model deployments in Research', () => {
  it('creates exactly the reviewed request, and only stopped', async () => {
    const api = createFakeApi();
    serveDeployments(api, []);
    api.on('POST', '/api/v1/deployments', (request) => ({
      status: 201,
      body: { deployment: deploymentJson({ deployment_id: (request.body as Json).deployment_id }), created: true },
    }));
    const { user } = renderWithServices(<Research resultId={null} />, { api, research: serveResearch(), deployments: true });

    const form = await screen.findByRole('region', { name: 'Deploy to paper' });
    // The default selection is SPY, whose logistic model is fitted.
    await user.type(within(form).getByLabelText('Deployment ID'), 'spy-logistic');
    await user.type(within(form).getByLabelText('New paper account ID'), 'model-spy');
    // The research service's defaults, converted exactly.
    expect(within(form).getByLabelText('Confidence threshold (0 to 1)')).toHaveValue('0.5');
    await user.click(within(form).getByRole('button', { name: 'Review deployment' }));

    const review = within(form).getByLabelText('Review the deployment');
    expect(within(review).getByText('spy-logistic')).toBeInTheDocument();
    expect(within(review).getByText(/belongs to it for good/)).toBeInTheDocument();
    expect(within(review).getByText(/Nothing trades until you press Start/)).toBeInTheDocument();
    expect(within(form).queryByLabelText('Deployment ID')).not.toBeInTheDocument();
    await user.click(within(form).getByRole('button', { name: 'Create deployment' }));

    expect(await within(form).findByText('Deployment created, stopped')).toBeInTheDocument();
    const [sent] = api.requestsTo('POST', '/api/v1/deployments');
    expect(sent?.body).toEqual({
      deployment_id: 'spy-logistic',
      account_id: 'model-spy',
      starting_cash: '10000',
      threshold: '0.5',
      target_quantity: '1',
      decision_interval_ms: '1000',
      latency_ms: '1',
      max_signal_age_ms: '5000',
      max_orders_per_run: '20',
      max_consecutive_rejections: '3',
      experiment_id: EXPERIMENT_ID,
      instrument: 'SPY',
      model: 'logistic',
    });
    // Nothing was started.
    expect(api.requests.some((request) => request.path.endsWith('/start'))).toBe(false);
  });

  it('starts only after an explicit confirmation, shows decisions, and stops', async () => {
    const api = createFakeApi();
    let current = deploymentJson();
    api.on('GET', '/api/v1/deployments', () => ({
      status: 200,
      body: { enabled: true, deployments: [current], next_cursor: null },
    }));
    api.on('GET', '/api/v1/deployments/spy-logistic/decisions', {
      status: 200,
      body: { decisions: [decisionJson()], next_cursor: null },
    });
    api.on('POST', '/api/v1/deployments/spy-logistic/start', () => {
      current = deploymentJson({ state: 'running', state_reason: 'started', active: true, run_generation: '1' });
      return { status: 200, body: { deployment: current } };
    });
    api.on('POST', '/api/v1/deployments/spy-logistic/stop', () => {
      current = deploymentJson({ state: 'stopped', state_reason: 'stop_requested', run_generation: '1' });
      return { status: 200, body: { deployment: current } };
    });
    const { user } = renderWithServices(<Research resultId={null} />, { api, research: serveResearch(), deployments: true });

    const card = await screen.findByRole('article', { name: 'Deployment spy-logistic' });
    expect(within(card).getByText('Created; not started yet')).toBeInTheDocument();
    const start = within(card).getByRole('button', { name: 'Start' });
    expect(start).toBeDisabled();
    await user.click(within(card).getByLabelText('Let this model place simulated orders in model-spy'));
    await user.click(start);
    expect(await within(card).findByText('Running')).toBeInTheDocument();
    expect(api.requestsTo('POST', '/api/v1/deployments/spy-logistic/start')[0]?.body).toEqual({});
    expect(await within(card).findByText('buy 1')).toBeInTheDocument();
    expect(within(card).getByText('filled')).toBeInTheDocument();

    await user.click(within(card).getByRole('button', { name: 'Stop' }));
    expect(await within(card).findByText('Stopped on request')).toBeInTheDocument();
  });

  it('arms the kill switch before using it', async () => {
    const api = createFakeApi();
    serveDeployments(api, [deploymentJson({ state: 'running', state_reason: 'started', active: true })]);
    api.on('GET', '/api/v1/deployments/spy-logistic/decisions', { status: 200, body: { decisions: [], next_cursor: null } });
    api.on('POST', '/api/v1/deployments/kill-switch', {
      status: 200,
      body: { stopped: [deploymentJson({ state_reason: 'kill_switch' })] },
    });
    const { user } = renderWithServices(<Research resultId={null} />, { api, research: serveResearch(), deployments: true });
    const panel = await screen.findByRole('region', { name: 'Paper deployments' });
    await user.click(await within(panel).findByRole('button', { name: 'Stop all deployments…' }));
    expect(api.requestsTo('POST', '/api/v1/deployments/kill-switch')).toHaveLength(0);
    await user.click(within(panel).getByRole('button', { name: 'Stop every deployment now' }));
    await waitFor(() => {
      expect(api.requestsTo('POST', '/api/v1/deployments/kill-switch')).toHaveLength(1);
    });
    expect(await within(panel).findByText(/Stopped 1\./)).toBeInTheDocument();
  });

  it('does not claim a lost Start response proves the model did not start', async () => {
    const api = createFakeApi();
    let current = deploymentJson();
    api.on('GET', '/api/v1/deployments', () => ({
      status: 200,
      body: { enabled: true, deployments: [current], next_cursor: null },
    }));
    api.on('GET', '/api/v1/deployments/spy-logistic/decisions', {
      status: 200,
      body: { decisions: [], next_cursor: null },
    });
    api.on('POST', '/api/v1/deployments/spy-logistic/start', () => {
      current = deploymentJson({ state: 'running', state_reason: 'started', active: true, run_generation: '1' });
      throw new TypeError('response lost after the server started trading');
    });
    const { user } = renderWithServices(<Research resultId={null} />, {
      api, research: serveResearch(), deployments: true,
    });
    const card = await screen.findByRole('article', { name: 'Deployment spy-logistic' });
    await user.click(within(card).getByLabelText('Let this model place simulated orders in model-spy'));
    await user.click(within(card).getByRole('button', { name: 'Start' }));
    expect(await within(card).findByText('Start outcome unknown')).toBeInTheDocument();
    expect(within(card).queryByText('The deployment did not start')).not.toBeInTheDocument();
    expect(await within(card).findByText('Running')).toBeInTheDocument();
    expect(within(card).getByRole('button', { name: 'Stop' })).toBeEnabled();
  });

  it('blocks another Start after a timeout even while a delayed start still appears stopped', async () => {
    const api = createFakeApi();
    serveDeployments(api, [deploymentJson()]);
    api.on('GET', '/api/v1/deployments/spy-logistic/decisions', {
      status: 200, body: { decisions: [], next_cursor: null },
    });
    api.failNext('POST', '/api/v1/deployments/spy-logistic/start');
    api.on('POST', '/api/v1/deployments/spy-logistic/stop', {
      status: 200, body: { deployment: deploymentJson({ state_reason: 'stop_requested' }) },
    });
    const { user } = renderWithServices(<Research resultId={null} />, {
      api, research: serveResearch(), deployments: true,
    });
    const card = await screen.findByRole('article', { name: 'Deployment spy-logistic' });
    await user.click(within(card).getByLabelText('Let this model place simulated orders in model-spy'));
    await user.click(within(card).getByRole('button', { name: 'Start' }));
    expect(await within(card).findByText('Start outcome unknown')).toBeInTheDocument();
    expect(within(card).queryByRole('button', { name: 'Start' })).not.toBeInTheDocument();
    await user.click(within(card).getByRole('button', { name: 'Stop' }));
    expect(await within(card).findByRole('button', { name: 'Start' })).toBeDisabled();
    expect(within(card).queryByText('Start outcome unknown')).not.toBeInTheDocument();
    expect(api.requestsTo('POST', '/api/v1/deployments/spy-logistic/start')).toHaveLength(1);
  });

  it('does not claim a lost creation response proves no deployment was created', async () => {
    const api = createFakeApi();
    serveDeployments(api, []);
    api.failNext('POST', '/api/v1/deployments');
    const { user } = renderWithServices(<Research resultId={null} />, {
      api, research: serveResearch(), deployments: true,
    });
    const form = await screen.findByRole('region', { name: 'Deploy to paper' });
    await user.type(within(form).getByLabelText('Deployment ID'), 'spy-logistic');
    await user.type(within(form).getByLabelText('New paper account ID'), 'model-spy');
    await user.click(within(form).getByRole('button', { name: 'Review deployment' }));
    await user.click(within(form).getByRole('button', { name: 'Create deployment' }));
    expect(await within(form).findByText('Creation outcome unknown')).toBeInTheDocument();
    expect(within(form).getByText(/retry this same reviewed request with the same IDs/)).toBeInTheDocument();
    expect(within(form).queryByText('The deployment was not created')).not.toBeInTheDocument();
  });

  it('says so when the server has deployments disabled', async () => {
    const api = createFakeApi();
    serveDeployments(api, [], false);
    renderWithServices(<Research resultId={null} />, { api, research: serveResearch(), deployments: true });
    expect(await screen.findByText('Model deployments are disabled on this server')).toBeInTheDocument();
  });
});

describe('an account owned by a deployment', () => {
  it('offers no manual order ticket', async () => {
    const api = createFakeApi();
    serveAccount(api, { account: accountJson({ owner: { kind: 'model_deployment', deployment_id: 'spy-logistic' } }) });
    rememberSelectedAccount(ACCOUNT_ID);
    renderWithServices(<App />, { api });
    expect(await screen.findByText('Traded by model deployment spy-logistic')).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: /Review order/ })).not.toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'Manage the deployment in Research' })).toHaveAttribute('href', '#/research');
  });
});
