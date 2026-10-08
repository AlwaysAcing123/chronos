// Model deployment resources as the server renders them
// (services/paper_trading/src/deployment_http.cpp).
import type { Json } from './fixtures';
import { EXPERIMENT_ID } from './researchFixtures';

// A condition (ADR 0019), every time absent unless given.
export function conditionJson(code: string, overrides: Json = {}): Json {
  return {
    code,
    reason: null,
    observation_received_at: null,
    training_cutoff: null,
    next_eligible_received_at: null,
    not_before_received_at: null,
    expires_at: null,
    decision_sequence: null,
    observed_at_market: null,
    observed_at_wall: null,
    ...overrides,
  };
}

// A deployment as the server renders it (deployment_http.cpp). Unless a
// test gives one, its condition follows its lifecycle: a stopped
// deployment's is its reason, a running one is watching.
export function deploymentJson(overrides: Json = {}): Json {
  const state = typeof overrides.state === 'string' ? overrides.state : 'stopped';
  const reason = typeof overrides.state_reason === 'string' ? overrides.state_reason : 'created';
  return {
    activity: null,
    market: null,
    condition:
      state === 'running' ? conditionJson('watching', { reason: 'decided_hold' }) : conditionJson(state, { reason }),
    trading_mode: 'paper',
    deployment_id: 'spy-logistic',
    account_id: 'model-spy',
    creation_sequence: '1',
    state: 'stopped',
    state_reason: 'created',
    state_changed_at: '2026-10-04T15:00:00.000000000Z',
    created_at: '2026-10-04T15:00:00.000000000Z',
    active: false,
    run_generation: '0',
    stream_id: 'databento-equs-mini-mbp1-20260925-1330-1340',
    starting_cash: '10000',
    model: {
      reference: 'experiment',
      training_cutoff_basis: 'verified',
      experiment_id: EXPERIMENT_ID,
      name: 'logistic',
      instrument: 'SPY',
      parameters_sha256: 'a'.repeat(64),
      feature_set: 'top_of_book_baseline/1',
      training_cutoff: '2026-09-25T13:35:19.090266460Z',
    },
    policy: { name: 'long_flat_threshold/1', threshold: '0.55', target_quantity: '1' },
    timing: { decision_interval_ms: '1000', latency_ms: '1', max_signal_age_ms: '5000' },
    limits: { max_orders_per_run: '20', max_consecutive_rejections: '3', orders_this_run: '0', consecutive_rejections: '0' },
    watermark: null,
    next_decision_sequence: '1',
    pending_decision: null,
    version: '0',
    run: null,
    ...overrides,
  };
}

export function decisionJson(overrides: Json = {}): Json {
  return {
    decision_sequence: '1',
    run_generation: '1',
    state: 'recorded',
    observation: {
      stream_id: 'databento-equs-mini-mbp1-20260925-1330-1340',
      instrument: 'SPY',
      ingest_sequence: '72801',
      event_at: '2026-09-25T13:35:20.000000000Z',
      received_at: '2026-09-25T13:35:20.000100000Z',
    },
    decided_at: '2026-09-25T13:35:20.000100000Z',
    inputs: ['10000000', '-500000000', '0', '100000000', '300000000'],
    prediction: { class: 'up', probabilities: ['0.1', '0.2', '0.7'], confidence: '0.7' },
    position_before: '0',
    target_quantity: '1',
    policy_reason: 'order_scheduled',
    order: {
      client_order_id: 'spy-logistic-000000000001',
      side: 'buy',
      quantity: '1',
      not_before_received_at: '2026-09-25T13:35:20.001100000Z',
      expires_at: '2026-09-25T13:35:25.000100000Z',
    },
    resolution: 'filled',
    execution: { ingest_sequence: '72803', received_at: '2026-09-25T13:35:20.002000000Z' },
    resolved_at: '2026-09-25T13:35:20.002000000Z',
    fill: {
      count: '1',
      quantity: '1',
      notional: '768.94',
      fees: '0.01',
      price: '768.94',
      first_filled_at: '2026-09-25T13:35:20.002000000Z',
      last_filled_at: '2026-09-25T13:35:20.002000000Z',
    },
    timing: {
      signal_to_decision: { clock: 'market', nanoseconds: '0' },
      execution_quote_offset: { clock: 'market', nanoseconds: '1900000' },
      signal_to_resolution: { clock: 'market', nanoseconds: '1900000' },
      signal_to_fill: { clock: 'market', nanoseconds: '1900000' },
    },
    created_at: '2026-10-04T15:00:01.000000000Z',
    ...overrides,
  };
}
