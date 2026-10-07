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
import { conditionJson, decisionJson, deploymentJson } from '../../test/deploymentFixtures';
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
    expect(await within(card).findByText('Buy 1')).toBeInTheDocument();
    expect(within(card).getByRole('cell', { name: /Filled 1 @ 768\.94/ })).toBeInTheDocument();

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


describe('what a deployment is doing, and what it decided', () => {
  // One deployment and its saved decisions, served as given.
  async function card(deployment: Json, decisions: Json[] = []) {
    const api = createFakeApi();
    serveDeployments(api, [deployment]);
    api.on('GET', `/api/v1/deployments/${String(deployment.deployment_id)}/decisions`, {
      status: 200,
      body: { decisions, next_cursor: null },
    });
    const rendered = renderWithServices(<Research resultId={null} />, { api, research: serveResearch(), deployments: true });
    const found = await rendered.findByRole('article', { name: `Deployment ${String(deployment.deployment_id)}` });
    return { ...rendered, api, card: found };
  }

  const running = (condition: Json) =>
    deploymentJson({ state: 'running', state_reason: 'started', active: true, run_generation: '2', condition });

  it('explains a wait for the training cutoff with the times it uses', async () => {
    const { card: found } = await card(
      running(
        conditionJson('before_training_cutoff', {
          training_cutoff: '2026-09-25T13:35:19.090266460Z',
          observation_received_at: '2026-09-25T13:33:00.000000000Z',
          observed_at_market: '2026-09-25T13:33:00.000000000Z',
          observed_at_wall: '2026-10-04T21:30:00.000000000Z',
        }),
      ),
    );
    const now = within(found).getByText('Now', { selector: 'dt' }).nextElementSibling as HTMLElement;
    expect(now).toHaveTextContent('Waiting for the training cutoff');
    expect(now).toHaveTextContent('training cutoff 2026-09-25 13:35:19.090266460');
    expect(now).toHaveTextContent('server time');
    expect(within(now).getByText('before_training_cutoff')).toBeInTheDocument();
  });

  it('explains a pending order waiting for its latency', async () => {
    const { card: found } = await card(
      running(
        conditionJson('order_pending', {
          reason: 'awaiting_latency',
          decision_sequence: '4',
          not_before_received_at: '2026-09-25T13:36:00.001000000Z',
          expires_at: '2026-09-25T13:36:05.000000000Z',
        }),
      ),
      [decisionJson({ state: 'pending', resolution: null, execution: null, resolved_at: null, fill: null, run_generation: '2',
        timing: {
          signal_to_decision: { clock: 'market', nanoseconds: '0' },
          execution_quote_offset: { clock: 'market', unavailable: 'not_resolved' },
          signal_to_resolution: { clock: 'market', unavailable: 'not_resolved' },
          signal_to_fill: { clock: 'market', unavailable: 'not_resolved' },
        } })],
    );
    expect(within(found).getByText(/minimum latency after the signal/)).toBeInTheDocument();
    expect(found).toHaveTextContent('not before 2026-09-25 13:36:00.001000000');
    expect(await within(found).findByRole('cell', { name: /Pending waiting to be placed/ })).toBeInTheDocument();
    // Unavailable timing says why, never zero.
    expect(found).toHaveTextContent('to decision resolved not resolved yet · to fill not resolved yet');
  });

  it('shows a held prediction, a recorded order that did not fill, and a prior run', async () => {
    const held = decisionJson({
      decision_sequence: '3', run_generation: '1', state: 'held', order: null, resolution: null, execution: null,
      resolved_at: null, fill: null, policy_reason: 'hold_below_threshold',
      prediction: { class: 'up', probabilities: ['0.3', '0.2', '0.5'], confidence: '0.5' },
    });
    const unfilled = decisionJson({ decision_sequence: '2', run_generation: '1', resolution: 'cancelled', fill: null });
    const { card: found } = await card(running(conditionJson('watching', { reason: 'decided_hold' })), [held, unfilled]);
    expect(await within(found).findByRole('cell', { name: /Not filled\s*nothing executable\s*cancelled/ })).toBeInTheDocument();
    const latest = within(found).getByText('Latest saved prediction', { selector: 'dt' }).nextElementSibling as HTMLElement;
    expect(latest).toHaveTextContent(
      'up, model confidence 0.5 (not a probability of profit) · Hold: confidence was below the threshold · No order (run 1, an earlier run)',
    );
    // It names its observation: the decisions and the status refresh separately.
    expect(latest).toHaveTextContent('observation received 2026-09-25 13:35:20.000100000 (market clock)');
    expect(within(found).getAllByText('run 1')).toHaveLength(2);
  });

  it('does not label an abandoned decision resolution as order acceptance', async () => {
    const abandoned = decisionJson({
      state: 'abandoned', resolution: 'signal_expired', execution: null, fill: null,
      timing: {
        signal_to_decision: { clock: 'market', nanoseconds: '0' },
        execution_quote_offset: { clock: 'market', unavailable: 'no_execution' },
        signal_to_resolution: { clock: 'market', nanoseconds: '5001000000' },
        signal_to_fill: { clock: 'market', unavailable: 'no_fill' },
      },
    });
    const { card: found } = await card(running(conditionJson('watching')), [abandoned]);
    await within(found).findByRole('cell', { name: /Never placed/ });
    const timing = within(found).getByText('Latest order timing (market clock)', { selector: 'dt' }).nextElementSibling;
    expect(timing).not.toHaveTextContent('order accepted');
    expect(timing).toHaveTextContent('decision resolved 5001 ms');
  });

  it('puts a reconnecting source or a stale quote ahead of the latest prediction', async () => {
    const reconnecting = await card(running(conditionJson('source_reconnecting', { reason: 'reconnecting' })), [decisionJson()]);
    expect(within(reconnecting.card).getByText('Waiting for market data: the live source is reconnecting.')).toBeInTheDocument();
    reconnecting.unmount();
    const stale = await card(running(conditionJson('quote_stale')));
    expect(within(stale.card).getByText(/does not mean the market is closed/)).toBeInTheDocument();
  });

  it('says a stopped or faulted deployment is not trading, whatever it last saw', async () => {
    const stopped = await card(deploymentJson({ state: 'stopped', state_reason: 'server_restarted' }), [decisionJson()]);
    expect(within(stopped.card).getByText('Not trading. It trades again only if started.')).toBeInTheDocument();
    expect(within(stopped.card).getByText('Run 0: no counts in this server process')).toBeInTheDocument();
    stopped.unmount();
    const faulted = await card(deploymentJson({ state: 'faulted', state_reason: 'inference_timeout' }));
    expect(within(faulted.card).getByText('It stopped itself and does not trade until started again.')).toBeInTheDocument();
    expect(within(faulted.card).getByText('The model did not answer in time')).toBeInTheDocument();
  });

  it('marks cached status as possibly out of date when a refresh fails', async () => {
    const { api, queryClient, card: found } = await card(running(conditionJson('watching', { reason: 'decided_hold' })));
    api.on('GET', '/api/v1/deployments', {
      status: 503,
      body: { error: { code: 'persistence_unavailable', message: 'PostgreSQL is unavailable', field: null, reason: null } },
    });
    await queryClient.refetchQueries({ queryKey: ['chronos-deployments', 'list'] });
    expect(await screen.findByText('This status may be out of date')).toBeInTheDocument();
    // The cached deployment stays, labelled, not replaced or confirmed.
    expect(found).toBeInTheDocument();
  });

  it('keeps counts by reason in details, scoped to the run and this process', async () => {
    const run = {
      candidates: '40', superseded: '3', skipped: { before_training_cutoff: '30', decision_interval: '5' },
      predictions: '5', holds: '4', order_decisions: '1', orders_recorded: '1', abandoned: '0',
      waits: { awaiting_latency: '2' }, last_inference_us: '300', max_inference_us: '2100',
      last_submission_us: '4000', max_submission_us: '61800', last_error: null,
    };
    const { card: found, user } = await card({ ...running(conditionJson('watching', { reason: 'decided_hold' })), run });
    const counts = within(found).getByText('Counts by reason (run 2, this server process)');
    await user.click(counts);
    expect(within(found).getByText('before_training_cutoff')).toBeVisible();
    expect(found).toHaveTextContent('trusted order path up to 61.8 ms, including waits for the market');
    expect(found).toHaveTextContent('inference up to 2.1 ms');
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
