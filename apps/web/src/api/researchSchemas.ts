// Runtime decoders for the local research service (docs/research-service.md).
// The same rules as the trading API's: decimals and integers are strings,
// timestamps are exact RFC 3339, and a response that does not match is an
// unexpected response rather than something to display.
import { z } from 'zod';

import {
  MoneySchema,
  PercentageSchema,
  PriceSchema,
  QuantitySchema,
  SideSchema,
  TimestampSchema,
  UnsignedIntegerSchema,
} from './schemas';

export const ArtifactIdSchema = z.string().regex(/^[0-9a-f]{64}$/, 'expected a 64-digit artifact ID');
export type ArtifactId = z.infer<typeof ArtifactIdSchema>;

export const JobIdSchema = z.string().regex(/^[0-9a-f]{32}$/, 'expected a 32-digit job ID');

export const ClassSchema = z.enum(['down', 'flat', 'up']);
export const ModelNameSchema = z.enum(['logistic', 'majority']);
export type ModelName = z.infer<typeof ModelNameSchema>;
export const WindowNameSchema = z.enum(['validation', 'test']);
export type WindowName = z.infer<typeof WindowNameSchema>;
export const AccountNameSchema = z.enum(['strategy', 'buy_and_hold', 'cash']);
export type AccountName = z.infer<typeof AccountNameSchema>;

// A metric with a value, or why it has none.
const MetricSchema = z.union([z.object({ value: z.string() }), z.object({ undefined: z.string() })]);
export type Metric = z.infer<typeof MetricSchema>;

const PredictionMetricsSchema = z
  .union([
    z.object({ accuracy: MetricSchema, balanced_accuracy: MetricSchema, rows: UnsignedIntegerSchema }),
    z.object({ unevaluable: z.string() }),
  ])
  .nullable();
export type PredictionMetrics = z.infer<typeof PredictionMetricsSchema>;

export const ModelStatusSchema = z.object({
  status: z.enum(['fitted', 'unevaluable']),
  reason: z.string().nullable().optional(),
  iterations: UnsignedIntegerSchema.optional(),
  majority_class: ClassSchema.optional(),
});
export type ModelStatus = z.infer<typeof ModelStatusSchema>;

const ExperimentInstrumentSchema = z.object({
  instrument: z.string().min(1),
  models: z.object({ logistic: ModelStatusSchema, majority: ModelStatusSchema }),
  prediction_metrics: z.object({
    validation: z.object({ logistic: PredictionMetricsSchema, majority: PredictionMetricsSchema }),
    test: z.object({ logistic: PredictionMetricsSchema, majority: PredictionMetricsSchema }),
  }),
  training: z.object({ rows: UnsignedIntegerSchema, true_distribution: z.record(z.string(), UnsignedIntegerSchema) }),
});
export type ExperimentInstrument = z.infer<typeof ExperimentInstrumentSchema>;

const IntervalSchema = z.object({ start_at: TimestampSchema, end_at: TimestampSchema.nullable() });

export const ExperimentSchema = z.object({
  experiment_id: ArtifactIdSchema,
  split_artifact_id: ArtifactIdSchema,
  horizon_ns: UnsignedIntegerSchema,
  scope: z.string(),
  verified: z.string(),
  limitations: z.array(z.string()),
  dataset: z.object({
    dataset_id: ArtifactIdSchema,
    events: UnsignedIntegerSchema,
    instruments: z.array(z.string()),
    provider: z.string(),
    provider_dataset: z.string(),
    stream: z.string(),
    receive_time: z.object({ earliest: TimestampSchema, latest: TimestampSchema }),
  }),
  instruments: z.array(ExperimentInstrumentSchema),
  windows: z.object({
    training: z.object({
      start_at: TimestampSchema,
      end_at: TimestampSchema,
      labels_available_through: TimestampSchema,
    }),
    validation: IntervalSchema,
    test: IntervalSchema,
  }),
});
export type Experiment = z.infer<typeof ExperimentSchema>;

export const ExperimentEnvelopeSchema = z.object({ experiment: ExperimentSchema });

export const ExperimentPageSchema = z.object({
  experiments: z.array(ExperimentSchema),
  invalid_entries: z.array(z.object({ experiment_id: z.string(), reason: z.string() })),
  next_cursor: z.string().nullable(),
});
export type ExperimentPage = z.infer<typeof ExperimentPageSchema>;

export const ConfigurationSchema = z.object({
  threshold_ppb: UnsignedIntegerSchema,
  target_quantity: z.string(),
  latency_ns: UnsignedIntegerSchema,
  decision_interval_ns: UnsignedIntegerSchema,
  starting_cash: z.string(),
  fee_per_fill: z.string(),
  maximum_quote_age_ns: UnsignedIntegerSchema,
  maximum_order_notional: z.string(),
});
export type RunConfiguration = z.infer<typeof ConfigurationSchema>;

export const StatusSchema = z.object({
  service: z.literal('chronos-research'),
  version: z.string(),
  defaults: ConfigurationSchema,
  limits: z.object({
    job_timeout_s: UnsignedIntegerSchema,
    max_queued: UnsignedIntegerSchema,
    max_running: UnsignedIntegerSchema,
    retain_jobs: UnsignedIntegerSchema,
  }),
  tools: z.record(z.string(), z.enum(['available', 'missing'])),
});
export type ResearchStatus = z.infer<typeof StatusSchema>;

export const RunRequestSchema = z.object({
  experiment_id: ArtifactIdSchema,
  instrument: z.string(),
  model: ModelNameSchema,
  window: WindowNameSchema,
  configuration: ConfigurationSchema,
});
export type RunRequest = z.infer<typeof RunRequestSchema>;

export const JobStateSchema = z.enum(['queued', 'running', 'succeeded', 'failed', 'interrupted']);
export type JobState = z.infer<typeof JobStateSchema>;

export const JobSchema = z.object({
  job_id: JobIdSchema,
  state: JobStateSchema,
  created_at: TimestampSchema,
  started_at: TimestampSchema.nullable(),
  finished_at: TimestampSchema.nullable(),
  request: RunRequestSchema,
  result: z
    .object({
      run_id: ArtifactIdSchema,
      schedule_id: ArtifactIdSchema,
      reproduced: z.string(),
    })
    .nullable(),
  error: z.object({ code: z.string(), message: z.string() }).nullable(),
  log: z.string(),
});
export type Job = z.infer<typeof JobSchema>;

export const StartedJobSchema = z.object({ job: JobSchema, deduplicated: z.boolean() });
export const JobEnvelopeSchema = z.object({ run: JobSchema });
export const JobPageSchema = z.object({ runs: z.array(JobSchema), next_cursor: z.string().nullable() });
export type JobPage = z.infer<typeof JobPageSchema>;

const UnavailableValuationSchema = z.object({
  unavailable: z.object({
    code: z.string(),
    message: z.string(),
    instruments: z.array(z.object({ instrument: z.string(), reason: z.string(), message: z.string() })),
  }),
});

const CompleteValuationSchema = z.object({
  cash: MoneySchema,
  equity: MoneySchema,
  fees_paid: MoneySchema,
  gross_realized_pnl: MoneySchema,
  gross_unrealized_pnl: MoneySchema,
  market_value: MoneySchema,
  net_total_pnl: MoneySchema,
  position: QuantitySchema,
  return_percent: PercentageSchema,
  mark: z
    .object({ bid: PriceSchema, ingest_sequence: UnsignedIntegerSchema, receive_timestamp: TimestampSchema })
    .nullable(),
});
export type CompleteValuation = z.infer<typeof CompleteValuationSchema>;

export const ValuationSchema = z.union([UnavailableValuationSchema, CompleteValuationSchema]);
export type Valuation = z.infer<typeof ValuationSchema>;

export function isComplete(valuation: Valuation): valuation is CompleteValuation {
  return !('unavailable' in valuation);
}

const AccountResultSchema = z.object({
  drawdown: z.object({
    maximum: MoneySchema,
    maximum_percent_of_peak: PercentageSchema.nullable(),
    peak_at: TimestampSchema.nullable(),
    trough_at: TimestampSchema.nullable(),
    points_valued: UnsignedIntegerSchema,
    points_unvalued: UnsignedIntegerSchema,
  }),
  fills: UnsignedIntegerSchema,
  final: z.object({
    ledger: z.object({
      cash: MoneySchema,
      fees_paid: MoneySchema,
      gross_realized_pnl: MoneySchema,
      open_cost_basis: MoneySchema,
      position: QuantitySchema,
    }),
    valuation: ValuationSchema,
  }),
  orders: z.object({
    scheduled: UnsignedIntegerSchema,
    filled: UnsignedIntegerSchema,
    partially_filled_and_cancelled: UnsignedIntegerSchema,
    cancelled: UnsignedIntegerSchema,
    rejected: UnsignedIntegerSchema,
    expired: UnsignedIntegerSchema,
    rejections: z.record(z.string(), UnsignedIntegerSchema),
  }),
  traded_quantity: QuantitySchema,
  turnover: z.object({ notional: MoneySchema, percent_of_starting_cash: PercentageSchema }),
});
export type AccountResult = z.infer<typeof AccountResultSchema>;

export const ResultSchema = z.object({
  run_id: ArtifactIdSchema,
  scope: z.string(),
  limitations: z.array(z.string()),
  assumptions: z.array(z.string()),
  definitions: z.record(z.string(), z.string()),
  environment: z.record(z.string(), z.string()),
  runner: z.object({ name: z.string(), version: z.string() }),
  journal_entries: UnsignedIntegerSchema,
  accounts: z.object({ strategy: AccountResultSchema, buy_and_hold: AccountResultSchema, cash: AccountResultSchema }),
  configuration: z.object({
    execution: z.object({
      fee_per_fill: MoneySchema,
      latency_ns: UnsignedIntegerSchema,
      maximum_order_notional: MoneySchema,
      maximum_quote_age_ns: UnsignedIntegerSchema,
      order_type: z.string(),
      time_in_force: z.string(),
    }),
    policy: z.object({
      decision_interval_ns: UnsignedIntegerSchema,
      name: z.string(),
      target_quantity: QuantitySchema,
      threshold_ppb: UnsignedIntegerSchema,
      version: z.string(),
    }),
    starting_cash: MoneySchema,
  }),
  decisions: z.object({
    evaluated: UnsignedIntegerSchema,
    orders: UnsignedIntegerSchema,
    already_at_target: UnsignedIntegerSchema,
    hold_below_threshold: UnsignedIntegerSchema,
    hold_flat_prediction: UnsignedIntegerSchema,
    order_pending: UnsignedIntegerSchema,
    skipped_decision_interval: UnsignedIntegerSchema,
    skipped_prediction_unavailable: UnsignedIntegerSchema,
  }),
  events: z.object({
    read: UnsignedIntegerSchema,
    warmup: UnsignedIntegerSchema,
    window: UnsignedIntegerSchema,
    after_window_unused: UnsignedIntegerSchema,
    schedule_rows: UnsignedIntegerSchema,
  }),
  source: z.object({
    dataset_id: ArtifactIdSchema,
    events_sha256: z.string(),
    experiment_id: ArtifactIdSchema,
    feature_artifact_id: ArtifactIdSchema,
    instrument: z.string(),
    label_artifact_id: ArtifactIdSchema,
    model: ModelNameSchema,
    parameters_sha256: z.string(),
    schedule_id: ArtifactIdSchema,
    schedule_sha256: z.string(),
    split_artifact_id: ArtifactIdSchema,
    stream: z.string(),
    training_labels_available_through: TimestampSchema,
  }),
  window: z.object({
    name: WindowNameSchema,
    start_at: TimestampSchema,
    end_at: TimestampSchema.nullable(),
    first_event_at: TimestampSchema.nullable(),
    valued_at: TimestampSchema.nullable(),
  }),
});
export type ResearchResult = z.infer<typeof ResultSchema>;
export const ResultEnvelopeSchema = z.object({ result: ResultSchema });

export const ResultListingSchema = z.object({
  run_id: ArtifactIdSchema,
  experiment_id: ArtifactIdSchema,
  instrument: z.string(),
  model: ModelNameSchema,
  net_total_pnl: MoneySchema.nullable(),
  valuation: z.enum(['complete', 'unavailable']),
  window: z.object({ name: WindowNameSchema, start_at: TimestampSchema, end_at: TimestampSchema.nullable() }),
});
export type ResultListing = z.infer<typeof ResultListingSchema>;
export const ResultPageSchema = z.object({
  results: z.array(ResultListingSchema),
  invalid_entries: z.array(z.object({ run_id: z.string(), reason: z.string() })),
  next_cursor: z.string().nullable(),
});
export type ResultPage = z.infer<typeof ResultPageSchema>;

const LevelSchema = z.object({ price: PriceSchema, quantity: QuantitySchema }).nullable();

export const TradeSchema = z.object({
  account: AccountNameSchema,
  client_order_id: z.string(),
  side: SideSchema,
  quantity: QuantitySchema,
  outcome: z.enum(['filled', 'partially_filled_and_cancelled', 'cancelled', 'rejected', 'expired']),
  signal_at: TimestampSchema,
  arrival_at: TimestampSchema,
  executed_at: TimestampSchema.optional(),
  fill: z
    .object({ price: PriceSchema, quantity: QuantitySchema, fee: MoneySchema, notional: MoneySchema, fill_id: z.string() })
    .nullable()
    .optional(),
  rejection: z.object({ by: z.string(), code: z.string(), message: z.string() }).nullable().optional(),
  cancellation_reason: z.string().nullable().optional(),
  observation: z
    .object({
      ask: LevelSchema,
      bid: LevelSchema,
      ingest_sequence: UnsignedIntegerSchema,
      receive_timestamp: TimestampSchema,
      suspect: z.boolean(),
    })
    .optional(),
  position_after: QuantitySchema.optional(),
  cash_after: MoneySchema.optional(),
  reason: z.string().optional(),
});
export type Trade = z.infer<typeof TradeSchema>;
export const TradePageSchema = z.object({ trades: z.array(TradeSchema), next_cursor: z.string().nullable() });
export type TradePage = z.infer<typeof TradePageSchema>;

export const DecisionSchema = z.object({
  account: AccountNameSchema,
  decision: UnsignedIntegerSchema,
  position_before: QuantitySchema,
  reason: z.string(),
  target: QuantitySchema.nullable(),
  signal: z.object({
    available_at: TimestampSchema,
    class: ClassSchema,
    confidence_ppb: UnsignedIntegerSchema,
    ingest_sequence: UnsignedIntegerSchema,
  }),
});
export type Decision = z.infer<typeof DecisionSchema>;
export const DecisionPageSchema = z.object({ decisions: z.array(DecisionSchema), next_cursor: z.string().nullable() });
export type DecisionPage = z.infer<typeof DecisionPageSchema>;

export const EquityPointSchema = z.object({
  at: TimestampSchema,
  trigger: z.enum(['decision', 'execution', 'window_end']),
  accounts: z.object({ strategy: ValuationSchema, buy_and_hold: ValuationSchema, cash: ValuationSchema }),
});
export type EquityPoint = z.infer<typeof EquityPointSchema>;
export const EquityPageSchema = z.object({ points: z.array(EquityPointSchema), next_cursor: z.string().nullable() });
