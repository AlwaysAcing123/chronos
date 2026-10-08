// Research service resources in contract form, shared by the component
// tests and the mocked browser suite. The numbers follow the C++ strategy
// tests' hand-checked session: one share bought at the 100.1 ask and sold at
// the 100.25 bid with a 0.01 fee per fill, from 10000.
import type { Json } from './fixtures';

export const EXPERIMENT_ID = 'e'.repeat(64);
export const RUN_ID = 'a'.repeat(64);
export const SCHEDULE_ID = 'b'.repeat(64);
export const JOB_ID = 'c'.repeat(32);
export const DATASET_ID = 'd'.repeat(64);

export const DEFAULT_CONFIGURATION = {
  decision_interval_ns: '1000000000',
  fee_per_fill: '0.01',
  latency_ns: '1000000',
  maximum_order_notional: '1000000',
  maximum_quote_age_ns: '5000000000',
  starting_cash: '10000',
  target_quantity: '1',
  threshold_ppb: '500000000',
};

// The ONNX import profile and sandbox, as the service reports them.
export const SANDBOX_LIMITS = {
  cpus: '1',
  load_timeout_s: '60',
  memory: '256m',
  nofile: '64',
  pids: '32',
  predict_timeout_ms: '2000',
  reply_bytes: '65536',
};

export const SANDBOX_AVAILABLE: Json = {
  content_sha256: '0f2742da590ccc4f54a2acc7c8dba6c3d9b52c6d2f5c94e384ef31daaa299f15',
  image: 'chronos-onnx-sandbox:0f2742da590ccc4f',
  limits: SANDBOX_LIMITS,
  status: 'available',
};

export const SANDBOX_UNAVAILABLE: Json = {
  detail:
    'the sandbox image chronos-onnx-sandbox:0f2742da590ccc4f is not built; run python -m chronos_research onnx-sandbox build',
  limits: SANDBOX_LIMITS,
  reason: 'image_not_built',
  status: 'unavailable',
};

export function importsJson(sandbox: Json = SANDBOX_AVAILABLE): Json {
  return {
    profile: {
      attributes: {
        Gemm: { alpha: 'FLOAT', beta: 'FLOAT', transA: 'INT', transB: 'INT' },
        Softmax: { axis: 'INT' },
      },
      input: { dtype: 'float64', name: 'inputs', shape: ['1', '5'] },
      ir_versions: ['7', '10'],
      max_initializer_elements: '16384',
      max_initializers: '32',
      max_model_bytes: '1048576',
      max_nodes: '64',
      max_tensor_elements: '4096',
      operators: ['Add', 'Div', 'Gemm', 'Identity', 'MatMul', 'Mul', 'Softmax', 'Sub'],
      opsets: ['13', '21'],
      output: { dtype: 'float64', name: 'probabilities', shape: ['1', '3'] },
      profile: { name: 'chronos.onnx_classifier', version: '1' },
    },
    sandbox,
  };
}

export function researchStatusJson(overrides: Json = {}): Json {
  return {
    defaults: DEFAULT_CONFIGURATION,
    imports: importsJson(),
    jobs: { failed: '0', interrupted: '0', queued: '0', running: '0', succeeded: '0' },
    limits: { job_timeout_s: '1800', max_queued: '8', max_running: '1', retain_jobs: '200' },
    service: 'chronos-research',
    tools: { chronos_dataset: 'available', chronos_strategy: 'available' },
    version: '1',
    ...overrides,
  };
}

const metrics = (accuracy: string, balanced: string, rows: string): Json => ({
  accuracy: { value: accuracy },
  balanced_accuracy: { value: balanced },
  rows,
});

export function experimentJson(overrides: Json = {}): Json {
  return {
    dataset: {
      dataset_id: DATASET_ID,
      events: '121303',
      instruments: ['AAPL', 'SPY'],
      provider: 'databento',
      provider_dataset: 'EQUS.MINI',
      receive_time: { earliest: '2026-09-25T13:30:00.001665218Z', latest: '2026-09-25T13:39:59.969574414Z' },
      stream: 'databento-equs-mini-mbp1-20260925-1330-1340',
    },
    experiment_id: EXPERIMENT_ID,
    horizon_ns: '1000000000',
    instruments: [
      {
        instrument: 'AAPL',
        models: {
          logistic: { reason: 'insufficient_class_support', status: 'unevaluable' },
          majority: { majority_class: 'down', status: 'fitted' },
        },
        prediction_metrics: {
          test: { logistic: { unevaluable: 'insufficient_class_support' }, majority: metrics('0.45', '0.3333333333333333', '5866') },
          validation: {
            logistic: { unevaluable: 'insufficient_class_support' },
            majority: metrics('0.43661971830985913', '0.3333333333333333', '4828'),
          },
        },
        training: { rows: '16919', true_distribution: { down: '8426', flat: '795', up: '7698' } },
      },
      {
        instrument: 'SPY',
        models: {
          logistic: { iterations: '60', status: 'fitted' },
          majority: { majority_class: 'up', status: 'fitted' },
        },
        prediction_metrics: {
          test: { logistic: metrics('0.52', '0.37', '17744'), majority: metrics('0.5', '0.3333333333333333', '17744') },
          validation: { logistic: metrics('0.51', '0.36', '18918'), majority: metrics('0.49', '0.3333333333333333', '18918') },
        },
        training: { rows: '54259', true_distribution: { down: '24046', flat: '2868', up: '27345' } },
      },
    ],
    limitations: [
      'Each dataset is one short recorded session: its validation and test windows cover minutes, not market regimes.',
      'No result here is evidence that a model or policy would be profitable.',
    ],
    scope: 'predictive evaluation only',
    split_artifact_id: 'f'.repeat(64),
    verified: 'the manifests and checksums',
    windows: {
      test: { end_at: null, start_at: '2026-09-25T13:37:46.577861679Z' },
      training: {
        end_at: '2026-09-25T13:35:19.090266460Z',
        labels_available_through: '2026-09-25T13:35:19.086964037Z',
        start_at: '2026-09-25T13:30:00.001665218Z',
      },
      validation: { end_at: '2026-09-25T13:37:46.577861679Z', start_at: '2026-09-25T13:35:19.090266460Z' },
    },
    ...overrides,
  };
}

export function experimentPageJson(experiments: Json[] = [experimentJson()], invalid: Json[] = []): Json {
  return { experiments, invalid_entries: invalid, next_cursor: null };
}

export function runRequestJson(overrides: Json = {}): Json {
  return {
    configuration: DEFAULT_CONFIGURATION,
    experiment_id: EXPERIMENT_ID,
    instrument: 'SPY',
    model: 'logistic',
    window: 'validation',
    ...overrides,
  };
}

export function jobJson(overrides: Json = {}): Json {
  return {
    created_at: '2026-10-04T05:40:00.000000000Z',
    error: null,
    finished_at: null,
    job_id: JOB_ID,
    log: '',
    request: runRequestJson(),
    result: null,
    started_at: null,
    state: 'queued',
    ...overrides,
  };
}

export function succeededJobJson(overrides: Json = {}): Json {
  return jobJson({
    finished_at: '2026-10-04T05:40:21.000000000Z',
    result: { reproduced: 'ran the schedule again over its verified dataset: identical journal and results', run_id: RUN_ID, schedule_id: SCHEDULE_ID },
    started_at: '2026-10-04T05:40:00.100000000Z',
    state: 'succeeded',
    ...overrides,
  });
}

function complete(fields: Json): Json {
  return {
    cash: '10000',
    equity: '10000',
    fees_paid: '0',
    gross_realized_pnl: '0',
    gross_unrealized_pnl: '0',
    mark: null,
    market_value: '0',
    net_total_pnl: '0',
    position: '0',
    return_percent: '0',
    ...fields,
  };
}

export const STALE_VALUATION: Json = {
  unavailable: {
    code: 'position_mark_unavailable',
    instruments: [{ instrument: 'SPY', message: 'top-of-book state is older than the limit', reason: 'stale_market_state' }],
    message: '1 open position cannot be marked',
  },
};

function account(fields: Json): Json {
  return {
    drawdown: {
      maximum: '0',
      maximum_percent_of_peak: null,
      peak_at: null,
      points_unvalued: '0',
      points_valued: '4',
      trough_at: null,
    },
    fills: '0',
    final: {
      ledger: { cash: '10000', fees_paid: '0', gross_realized_pnl: '0', open_cost_basis: '0', position: '0' },
      valuation: complete({}),
    },
    orders: { cancelled: '0', expired: '0', filled: '0', partially_filled_and_cancelled: '0', rejected: '0', rejections: {}, scheduled: '0' },
    traded_quantity: '0',
    turnover: { notional: '0', percent_of_starting_cash: '0' },
    ...fields,
  };
}

export function resultJson(overrides: Json = {}): Json {
  return {
    accounts: {
      buy_and_hold: account({
        drawdown: {
          maximum: '0.61',
          maximum_percent_of_peak: '0.0061',
          peak_at: '2026-09-25T13:35:19.100000000Z',
          points_unvalued: '1',
          points_valued: '3',
          trough_at: '2026-09-25T13:35:20.100000000Z',
        },
        fills: '1',
        final: {
          ledger: { cash: '9899.89', fees_paid: '0.01', gross_realized_pnl: '0', open_cost_basis: '100.1', position: '1' },
          valuation: STALE_VALUATION,
        },
        orders: { cancelled: '0', expired: '0', filled: '1', partially_filled_and_cancelled: '0', rejected: '0', rejections: {}, scheduled: '1' },
        traded_quantity: '1',
        turnover: { notional: '100.1', percent_of_starting_cash: '1.001' },
      }),
      cash: account({}),
      strategy: account({
        drawdown: {
          maximum: '0.61',
          maximum_percent_of_peak: '0.0061',
          peak_at: '2026-09-25T13:35:19.100000000Z',
          points_unvalued: '0',
          points_valued: '4',
          trough_at: '2026-09-25T13:35:20.100000000Z',
        },
        fills: '2',
        final: {
          ledger: { cash: '10000.13', fees_paid: '0.02', gross_realized_pnl: '0.15', open_cost_basis: '0', position: '0' },
          valuation: complete({ cash: '10000.13', equity: '10000.13', fees_paid: '0.02', gross_realized_pnl: '0.15', net_total_pnl: '0.13', return_percent: '0.0013' }),
        },
        orders: { cancelled: '0', expired: '1', filled: '2', partially_filled_and_cancelled: '0', rejected: '0', rejections: {}, scheduled: '3' },
        traded_quantity: '2',
        turnover: { notional: '200.35', percent_of_starting_cash: '2.0035' },
      }),
    },
    assumptions: ['An order arrives latency_ns after its signal.'],
    configuration: {
      execution: {
        fee_per_fill: '0.01',
        latency_ns: '1000000',
        maximum_order_notional: '1000000',
        maximum_quote_age_ns: '5000000000',
        order_type: 'market',
        time_in_force: 'immediate_or_cancel',
      },
      policy: { decision_interval_ns: '1000000000', name: 'long_flat_threshold', target_quantity: '1', threshold_ppb: '500000000', version: '1' },
      starting_cash: '10000',
    },
    decisions: {
      already_at_target: '1',
      evaluated: '3',
      hold_below_threshold: '0',
      hold_flat_prediction: '0',
      order_pending: '0',
      orders: '2',
      skipped_decision_interval: '7',
      skipped_prediction_unavailable: '1',
    },
    definitions: { drawdown: 'the largest fall of equity from its running peak' },
    environment: { compiler: 'clang 17', platform: 'darwin' },
    events: { after_window_unused: '0', read: '20', schedule_rows: '11', warmup: '8', window: '12' },
    journal_entries: '19',
    limitations: ['No result here is evidence that a model or policy would be profitable.'],
    run_id: RUN_ID,
    runner: { name: 'chronos.strategy.event_loop', version: '1' },
    scope: 'a deterministic paper simulation of one policy',
    source: {
      dataset_id: DATASET_ID,
      events_sha256: '1'.repeat(64),
      experiment_id: EXPERIMENT_ID,
      feature_artifact_id: '2'.repeat(64),
      instrument: 'SPY',
      label_artifact_id: '3'.repeat(64),
      model: 'logistic',
      parameters_sha256: '4'.repeat(64),
      schedule_id: SCHEDULE_ID,
      schedule_sha256: '5'.repeat(64),
      split_artifact_id: 'f'.repeat(64),
      stream: 'databento-equs-mini-mbp1-20260925-1330-1340',
      training_labels_available_through: '2026-09-25T13:35:19.086964037Z',
    },
    window: {
      end_at: '2026-09-25T13:37:46.577861679Z',
      first_event_at: '2026-09-25T13:35:19.090266460Z',
      name: 'validation',
      start_at: '2026-09-25T13:35:19.090266460Z',
      valued_at: '2026-09-25T13:35:22.102000000Z',
    },
    ...overrides,
  };
}

export function resultListingJson(overrides: Json = {}): Json {
  return {
    experiment_id: EXPERIMENT_ID,
    instrument: 'SPY',
    model: 'logistic',
    net_total_pnl: '0.13',
    run_id: RUN_ID,
    valuation: 'complete',
    window: { end_at: '2026-09-25T13:37:46.577861679Z', name: 'validation', start_at: '2026-09-25T13:35:19.090266460Z' },
    ...overrides,
  };
}

export function equityPointsJson(): Json[] {
  const point = (at: string, trigger: string, strategy: Json, buyAndHold: Json): Json => ({
    accounts: { buy_and_hold: buyAndHold, cash: complete({}), strategy },
    at,
    trigger,
  });
  return [
    point('2026-09-25T13:35:19.100000000Z', 'decision', complete({}), complete({})),
    point('2026-09-25T13:35:19.102000000Z', 'execution', complete({ equity: '9999.89' }), complete({ equity: '9999.89', position: '1' })),
    point('2026-09-25T13:35:20.100000000Z', 'decision', complete({ equity: '9999.39' }), complete({ equity: '9999.39', position: '1' })),
    point('2026-09-25T13:35:22.102000000Z', 'window_end', complete({ equity: '10000.13' }), STALE_VALUATION),
  ];
}

const observation = (sequence: string, at: string): Json => ({
  ask: { price: '100.35', quantity: '10' },
  bid: { price: '100.25', quantity: '10' },
  ingest_sequence: sequence,
  receive_timestamp: at,
  suspect: false,
});

export function tradesJson(): Json[] {
  return [
    {
      account: 'strategy',
      arrival_at: '2026-09-25T13:35:19.101000000Z',
      cancellation_reason: null,
      cash_after: '9899.89',
      client_order_id: 'strategy-000001',
      executed_at: '2026-09-25T13:35:19.102000000Z',
      fill: { fee: '0.01', fill_id: 'strategy-000001:fill:0', notional: '100.1', price: '100.1', quantity: '1' },
      observation: observation('1', '2026-09-25T13:35:19.102000000Z'),
      outcome: 'filled',
      position_after: '1',
      quantity: '1',
      rejection: null,
      side: 'buy',
      signal_at: '2026-09-25T13:35:19.100000000Z',
    },
    {
      account: 'strategy',
      arrival_at: '2026-09-25T13:35:21.101000000Z',
      cancellation_reason: null,
      cash_after: '10000.13',
      client_order_id: 'strategy-000002',
      executed_at: '2026-09-25T13:35:21.102000000Z',
      fill: { fee: '0.01', fill_id: 'strategy-000002:fill:0', notional: '100.25', price: '100.25', quantity: '1' },
      observation: observation('5', '2026-09-25T13:35:21.102000000Z'),
      outcome: 'filled',
      position_after: '0',
      quantity: '1',
      rejection: null,
      side: 'sell',
      signal_at: '2026-09-25T13:35:21.100000000Z',
    },
    {
      account: 'strategy',
      arrival_at: '2026-09-25T13:35:22.103000000Z',
      client_order_id: 'strategy-000003',
      outcome: 'expired',
      quantity: '1',
      reason: 'window_ended_before_an_eligible_observation',
      side: 'buy',
      signal_at: '2026-09-25T13:35:22.102000000Z',
    },
  ];
}

export function decisionsJson(): Json[] {
  const decision = (number: string, at: string, cls: string, confidence: string, before: string, target: string | null, reason: string): Json => ({
    account: 'strategy',
    decision: number,
    position_before: before,
    reason,
    signal: { available_at: at, class: cls, confidence_ppb: confidence, ingest_sequence: number },
    target,
  });
  return [
    decision('1', '2026-09-25T13:35:19.100000000Z', 'up', '900000000', '0', '1', 'order_scheduled'),
    decision('2', '2026-09-25T13:35:20.100000000Z', 'up', '612345678', '1', '1', 'already_at_target'),
    decision('3', '2026-09-25T13:35:21.100000000Z', 'down', '501539269', '1', '0', 'order_scheduled'),
  ];
}
