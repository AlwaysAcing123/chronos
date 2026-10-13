// Runtime decoders for strategy diagnostics (docs/strategy-diagnostics.md,
// ADR 0024): exact PnL attribution and decision cohorts of walk-forward runs.
// Attribution figures are the service's exact decimals of up to 18 places, a
// type of their own: the Money, Price, and Quantity schemas are unchanged.
// A missing figure is a reason, never a zero, and nothing here is computed.
import { z } from 'zod';

import { isCanonicalDecimal } from './decimal';
import { ArtifactIdSchema, ClassSchema, JobIdSchema, JobStateSchema, ModelNameSchema } from './researchSchemas';
import {
  MoneySchema,
  PercentageSchema,
  PriceSchema,
  QuantitySchema,
  TimestampSchema,
  UnsignedIntegerSchema,
} from './schemas';
import { EvidenceSchema, FoldWindowSchema, ScenarioSchema } from './walkForwardSchemas';

export const DIAGNOSTIC_PLACES = 18;

export const DiagnosticDecimalSchema = z
  .string()
  .refine((text) => isCanonicalDecimal(text, DIAGNOSTIC_PLACES), {
    message: `expected a canonical decimal string with at most ${String(DIAGNOSTIC_PLACES)} fractional digits`,
  })
  .brand<'DiagnosticDecimal'>();
export type DiagnosticDecimal = z.infer<typeof DiagnosticDecimalSchema>;

export const FigureSchema = z.union([
  z.object({ value: DiagnosticDecimalSchema }),
  z.object({ unavailable: z.string() }),
]);
export type Figure = z.infer<typeof FigureSchema>;

export const RatioSchema = z.union([
  z.object({ numerator: UnsignedIntegerSchema, denominator: UnsignedIntegerSchema, rounded: z.string() }),
  z.object({ undefined: z.string() }),
]);
export type Ratio = z.infer<typeof RatioSchema>;

export const COMPONENTS = [
  'booked_net_pnl',
  'midpoint_inventory_pnl',
  'execution_spread_cost',
  'fees',
  'terminal_bid_markdown',
  'rounding_adjustment',
  'market_value_rounding',
  'notional_rounding',
  'observed_midpoint_movement',
  'signal_reference_pnl',
  'inventory_pnl_at_bid',
] as const;
export type Component = (typeof COMPONENTS)[number];

export const ComponentsSchema = z.object({
  booked_net_pnl: FigureSchema,
  midpoint_inventory_pnl: FigureSchema,
  execution_spread_cost: FigureSchema,
  fees: FigureSchema,
  terminal_bid_markdown: FigureSchema,
  rounding_adjustment: FigureSchema,
  market_value_rounding: FigureSchema,
  notional_rounding: FigureSchema,
  observed_midpoint_movement: FigureSchema,
  signal_reference_pnl: FigureSchema,
  inventory_pnl_at_bid: FigureSchema,
});
export type Components = z.infer<typeof ComponentsSchema>;

export const SIMULATED_ACCOUNTS = ['strategy', 'buy_and_hold', 'cash'] as const;
export const SimulatedAccountSchema = z.enum(SIMULATED_ACCOUNTS);
export type SimulatedAccount = z.infer<typeof SimulatedAccountSchema>;

const CountsSchema = z.record(z.string(), UnsignedIntegerSchema);
const ClassCountsSchema = z.object({ down: UnsignedIntegerSchema, flat: UnsignedIntegerSchema, up: UnsignedIntegerSchema });
const Unavailable = z.object({ unavailable: z.string() });
const DurationsSchema = z.union([
  z.object({ maximum: UnsignedIntegerSchema, minimum: UnsignedIntegerSchema, sum: UnsignedIntegerSchema }),
  Unavailable,
]);
const SpreadsSchema = z.union([
  z.object({
    fills: UnsignedIntegerSchema,
    maximum: DiagnosticDecimalSchema,
    minimum: DiagnosticDecimalSchema,
    sum: DiagnosticDecimalSchema,
    without_both_sides: UnsignedIntegerSchema,
  }),
  Unavailable,
]);

const ReconciliationSchema = z.object({
  booked_net_pnl: z.union([z.object({ state: z.enum(['reconciled', 'reconciled_at_bid']) }), Unavailable]),
  signal_reference: z.union([z.object({ state: z.literal('reconciled') }), Unavailable]),
});
export type Reconciliation = z.infer<typeof ReconciliationSchema>;

export const DiagnosticAccountSchema = z.object({
  booked: z.object({
    cash: MoneySchema,
    fees_paid: MoneySchema,
    final_quantity: QuantitySchema,
    starting_cash: MoneySchema,
    valuation: z.union([
      z.object({
        equity: MoneySchema,
        market_value: MoneySchema,
        net_total_pnl: MoneySchema,
        return_percent: PercentageSchema,
      }),
      Unavailable,
    ]),
  }),
  components: ComponentsSchema,
  episodes: z.object({
    censored: UnsignedIntegerSchema,
    completed: UnsignedIntegerSchema,
    completed_duration_ns: DurationsSchema,
    completed_pnl: FigureSchema,
    completed_with_gain: UnsignedIntegerSchema,
    completed_with_loss: UnsignedIntegerSchema,
  }),
  fills: UnsignedIntegerSchema,
  final_mark: z.union([
    z.null(),
    Unavailable,
    z.object({
      ask: DiagnosticDecimalSchema.nullable(),
      bid: PriceSchema,
      ingest_sequence: UnsignedIntegerSchema,
      midpoint: FigureSchema,
      receive_at: TimestampSchema,
    }),
  ]),
  orders: z.object({
    cancellation_reasons: CountsSchema,
    outcomes: z.object({
      cancelled: UnsignedIntegerSchema,
      expired: UnsignedIntegerSchema,
      filled: UnsignedIntegerSchema,
      partially_filled_and_cancelled: UnsignedIntegerSchema,
      rejected: UnsignedIntegerSchema,
      scheduled: UnsignedIntegerSchema,
    }),
    rejections: CountsSchema,
  }),
  quoted_spreads: z.object({ execution: SpreadsSchema, signal: SpreadsSchema }),
  reconciliation: ReconciliationSchema,
  timing: z.object({
    configured_latency_ns: UnsignedIntegerSchema,
    executions: UnsignedIntegerSchema,
    expired: UnsignedIntegerSchema,
    arrival_to_execution_duration_ns: DurationsSchema,
    signal_to_arrival_duration_ns: DurationsSchema,
    signal_to_execution_duration_ns: DurationsSchema,
  }),
});
export type DiagnosticAccount = z.infer<typeof DiagnosticAccountSchema>;

export const COHORTS = ['scorable_predictions', 'sampled_decisions', 'order_scheduling_decisions', 'filled_orders'] as const;
export type CohortName = (typeof COHORTS)[number];

// Future returns in the label artifact's own unit, return_raw: parts per
// billion of the current midpoint. Exact integers and an exact mean.
const SignedIntegerSchema = z.string().regex(/^(0|-?[1-9][0-9]*)$/);
const ExtremeSchema = z.union([z.object({ value: SignedIntegerSchema }), z.object({ undefined: z.string() })]);
export const ReturnsSchema = z.object({
  rows: UnsignedIntegerSchema,
  sum_raw: SignedIntegerSchema,
  mean_raw: z.union([
    z.object({ numerator: SignedIntegerSchema, denominator: UnsignedIntegerSchema, rounded: z.string() }),
    z.object({ undefined: z.string() }),
  ]),
  minimum_raw: ExtremeSchema,
  maximum_raw: ExtremeSchema,
});
export type Returns = z.infer<typeof ReturnsSchema>;

export const CohortSchema = z.object({
  accuracy: RatioSchema,
  balanced_accuracy: RatioSchema,
  confidence_bins: z.array(
    z.object({
      accuracy: RatioSchema,
      correct: UnsignedIntegerSchema,
      items: UnsignedIntegerSchema,
      name: z.string(),
      scored: UnsignedIntegerSchema,
    }),
  ),
  confusion: z.record(ClassSchema, ClassCountsSchema),
  correct: UnsignedIntegerSchema,
  future_returns: z.object({
    all: ReturnsSchema,
    by_predicted_class: z.object({ down: ReturnsSchema, flat: ReturnsSchema, up: ReturnsSchema }),
    by_confidence_bin: z.array(ReturnsSchema.extend({ name: z.string() })),
    not_scored: UnsignedIntegerSchema,
  }),
  items: UnsignedIntegerSchema,
  labels: CountsSchema,
  predicted: ClassCountsSchema,
  realized: ClassCountsSchema,
  reasons: CountsSchema,
  scored: UnsignedIntegerSchema,
});
export type Cohort = z.infer<typeof CohortSchema>;

const DecisionCohortsSchema = z.object({
  sampled_decisions: CohortSchema,
  order_scheduling_decisions: CohortSchema,
  filled_orders: CohortSchema,
});
export type DecisionCohorts = z.infer<typeof DecisionCohortsSchema>;

export const CompletedRunSchema = z.object({
  state: z.literal('completed'),
  fold: UnsignedIntegerSchema,
  model: ModelNameSchema,
  scenario: z.string(),
  run_id: ArtifactIdSchema,
  journal_sha256: ArtifactIdSchema,
  fee_per_fill: MoneySchema,
  latency_ns: UnsignedIntegerSchema,
  accounts: z.object({
    strategy: DiagnosticAccountSchema,
    buy_and_hold: DiagnosticAccountSchema,
    cash: DiagnosticAccountSchema,
  }),
  deltas: z.object({ buy_and_hold: ComponentsSchema, cash: ComponentsSchema }),
  decisions: z.object({
    cohorts: DecisionCohortsSchema,
    evaluated: UnsignedIntegerSchema,
    journaled: CountsSchema,
    skipped_decision_interval: UnsignedIntegerSchema,
    skipped_prediction_unavailable: UnsignedIntegerSchema,
  }),
  engine_replay: z.object({ recorded_by: z.literal('study'), statement: z.string().nullable() }),
});
export type CompletedRun = z.infer<typeof CompletedRunSchema>;

export const DiagnosticRunSchema = z.discriminatedUnion('state', [
  CompletedRunSchema,
  z.object({
    state: z.literal('unavailable'),
    fold: UnsignedIntegerSchema,
    model: ModelNameSchema,
    scenario: z.string(),
    reason: z.string(),
  }),
]);
export type DiagnosticRun = z.infer<typeof DiagnosticRunSchema>;

const PredictionsSchema = z.object({
  cohort: CohortSchema,
  feature_unavailable: CountsSchema,
  scheduled: UnsignedIntegerSchema,
});

const DiagnosticModelSchema = z.discriminatedUnion('state', [
  z.object({
    state: z.literal('completed'),
    predictions: PredictionsSchema,
    schedule_id: ArtifactIdSchema,
    schedule_sha256: ArtifactIdSchema,
    study_scores: z.string(),
  }),
  z.object({ state: z.literal('unavailable'), reason: z.string() }),
]);
export type DiagnosticModel = z.infer<typeof DiagnosticModelSchema>;

export const DiagnosticFoldSchema = z.object({
  index: UnsignedIntegerSchema,
  dataset_id: ArtifactIdSchema,
  events_sha256: ArtifactIdSchema,
  feature_artifact_id: ArtifactIdSchema,
  features_sha256: ArtifactIdSchema,
  label_artifact_id: ArtifactIdSchema,
  labels_sha256: ArtifactIdSchema,
  stream: z.string(),
  window: FoldWindowSchema,
  models: z.object({ majority: DiagnosticModelSchema, logistic: DiagnosticModelSchema }),
  runs: z.array(DiagnosticRunSchema),
});
export type DiagnosticFold = z.infer<typeof DiagnosticFoldSchema>;

const PooledAccountsSchema = z.object({
  accounts: z.object({ strategy: ComponentsSchema, buy_and_hold: ComponentsSchema, cash: ComponentsSchema }),
  deltas: z.object({ buy_and_hold: ComponentsSchema, cash: ComponentsSchema }),
  cohorts: DecisionCohortsSchema,
  status: z.enum(['complete', 'partial', 'unavailable']),
});

export const TradingPoolSchema = PooledAccountsSchema.extend({
  folds: z.object({
    included: z.array(UnsignedIntegerSchema),
    missing: z.array(UnsignedIntegerSchema),
    planned: UnsignedIntegerSchema,
  }),
});
export type TradingPool = z.infer<typeof TradingPoolSchema>;
const StudiesCoveredSchema = z.object({ included: z.array(ArtifactIdSchema), missing: z.array(ArtifactIdSchema) });
export const AcrossPoolSchema = PooledAccountsSchema.extend({ studies: StudiesCoveredSchema });
export type AcrossPool = z.infer<typeof AcrossPoolSchema>;

export const StudySectionSchema = z.object({
  study_id: ArtifactIdSchema,
  plan_id: ArtifactIdSchema,
  collection_id: ArtifactIdSchema,
  evidence: EvidenceSchema,
  instrument: z.string(),
  scenarios: z.array(ScenarioSchema),
  status: z.enum(['complete', 'partial']),
  study_status: z.enum(['complete', 'partial']),
  missing: z.array(z.object({ fold: UnsignedIntegerSchema, item: z.string(), reason: z.string() })),
  folds: z.array(DiagnosticFoldSchema),
  pooled: z.object({
    predictions: z.record(
      ModelNameSchema,
      z.object({
        cohort: CohortSchema,
        folds: z.object({ included: z.array(UnsignedIntegerSchema), planned: UnsignedIntegerSchema }),
        status: z.enum(['complete', 'partial', 'unavailable']),
      }),
    ),
    trading: z.record(ModelNameSchema, z.record(z.string(), TradingPoolSchema)),
  }),
});
export type StudySection = z.infer<typeof StudySectionSchema>;

export const DiagnosticsSchema = z.object({
  diagnostic_id: ArtifactIdSchema,
  schema: z.object({ name: z.literal('chronos.strategy_diagnostics'), version: z.literal('2') }),
  status: z.enum(['complete', 'partial']),
  scope: z.string(),
  request: z.object({ study_ids: z.array(ArtifactIdSchema) }),
  counts: z.object({
    episodes: UnsignedIntegerSchema,
    fills: UnsignedIntegerSchema,
    runs: UnsignedIntegerSchema,
    runs_attributed: UnsignedIntegerSchema,
  }),
  confidence_bins: z.array(
    z.object({ lower_ppb: UnsignedIntegerSchema, name: z.string(), upper: z.string(), upper_ppb: UnsignedIntegerSchema }),
  ),
  definitions: z.object({
    attribution: z.record(z.string(), z.string()),
    identities: z.array(z.string()),
    symbols: z.string(),
    scales: z.string(),
    unavailable: z.string(),
    joins: z.string(),
    timing: z.string(),
    episodes: z.string(),
    cohorts: z.record(z.string(), z.string()),
    labels: z.string(),
    ratios: z.string(),
    future_returns: z.string(),
    pooling: z.string(),
    not_computed: z.string(),
  }),
  studies: z.array(StudySectionSchema),
  across_studies: z
    .object({
      studies: z.array(ArtifactIdSchema),
      scenarios: z.array(ScenarioSchema),
      trading: z.record(ModelNameSchema, z.record(z.string(), AcrossPoolSchema)),
      predictions: z.record(
        ModelNameSchema,
        z.object({
          cohort: CohortSchema,
          status: z.enum(['complete', 'partial', 'unavailable']),
          studies: StudiesCoveredSchema,
        }),
      ),
      unshared_scenarios: z.array(z.object({ scenario: z.string(), study_id: ArtifactIdSchema })),
    })
    .optional(),
});
export type Diagnostics = z.infer<typeof DiagnosticsSchema>;

const CommandLevelSchema = z.object({ command: z.string(), detail: z.string(), state: z.literal('not_run_by_this_read') });
const SourceStatesSchema = z.record(ArtifactIdSchema, z.string());

export const SOURCE_KINDS = ['studies', 'plans', 'collections', 'datasets', 'features', 'labels', 'schedules', 'runs'] as const;
export type SourceKind = (typeof SOURCE_KINDS)[number];

// A figure's trust: verified only if every source it depends on verified
// on this read; otherwise the sources that did not, as "kind:id".
export const TrustSchema = z.discriminatedUnion('state', [
  z.object({ state: z.literal('verified') }),
  z.object({ state: z.literal('invalid'), sources: z.array(z.string()).min(1) }),
]);
export type Trust = z.infer<typeof TrustSchema>;
const PoolTrustSchema = z.object({
  predictions: z.partialRecord(ModelNameSchema, TrustSchema),
  trading: z.partialRecord(ModelNameSchema, z.record(z.string(), TrustSchema)),
});

export const FigureTrustSchema = z.object({
  runs: z.record(ArtifactIdSchema, TrustSchema),
  studies: z.record(
    ArtifactIdSchema,
    z.object({
      folds: z.record(UnsignedIntegerSchema, z.partialRecord(ModelNameSchema, TrustSchema)),
      pooled: PoolTrustSchema,
    }),
  ),
  across_studies: PoolTrustSchema.optional(),
});
export type FigureTrust = z.infer<typeof FigureTrustSchema>;

export const DiagnosticsVerificationSchema = z.object({
  artifact: z.object({ detail: z.string(), state: z.literal('verified') }),
  sources: z.object({
    at_generation: z.string(),
    now: z.object({
      content: z.object({ hashed_now: UnsignedIntegerSchema, reused_unchanged: UnsignedIntegerSchema }),
      detail: z.string(),
      figures: FigureTrustSchema,
      sources: z.object({
        studies: SourceStatesSchema,
        plans: SourceStatesSchema,
        collections: SourceStatesSchema,
        datasets: SourceStatesSchema,
        features: SourceStatesSchema,
        labels: SourceStatesSchema,
        schedules: SourceStatesSchema,
        runs: SourceStatesSchema,
      }),
      state: z.enum(['verified', 'invalid']),
    }),
  }),
  recomputed_inspection: CommandLevelSchema,
  independent_audit: CommandLevelSchema,
  engine_replay: z.object({ detail: z.string(), state: z.literal('recorded_by_study') }),
});
export type DiagnosticsVerification = z.infer<typeof DiagnosticsVerificationSchema>;

export const DiagnosticsEnvelopeSchema = z.object({
  diagnostics: DiagnosticsSchema,
  verification: DiagnosticsVerificationSchema,
});
export type DiagnosticsDetail = z.infer<typeof DiagnosticsEnvelopeSchema>;

export const DiagnosticsListingSchema = z.object({
  diagnostic_id: ArtifactIdSchema,
  status: z.enum(['complete', 'partial']),
  counts: DiagnosticsSchema.shape.counts,
  studies: z.array(
    z.object({
      study_id: ArtifactIdSchema,
      instrument: z.string(),
      evidence: EvidenceSchema,
      missing: UnsignedIntegerSchema,
      status: z.enum(['complete', 'partial']),
    }),
  ),
});
export type DiagnosticsListing = z.infer<typeof DiagnosticsListingSchema>;

export const DiagnosticsPageSchema = z.object({
  diagnostics: z.array(DiagnosticsListingSchema),
  invalid_entries: z.array(z.object({ diagnostic_id: z.string(), reason: z.string() })),
  next_cursor: z.string().nullable(),
});
export type DiagnosticsPage = z.infer<typeof DiagnosticsPageSchema>;

const LOCATION = {
  study_id: ArtifactIdSchema,
  fold: UnsignedIntegerSchema,
  model: ModelNameSchema,
  scenario: z.string(),
  run_id: ArtifactIdSchema,
  account: SimulatedAccountSchema,
};
const LevelSchema = z.object({ price: PriceSchema, quantity: QuantitySchema }).nullable();

export const FillRowSchema = z.object({
  ...LOCATION,
  client_order_id: z.string(),
  fill_id: z.string(),
  decision: UnsignedIntegerSchema,
  side: z.enum(['buy', 'sell']),
  outcome: z.enum(['filled', 'partially_filled_and_cancelled']),
  quantity: QuantitySchema,
  price: PriceSchema,
  notional: MoneySchema,
  fee: MoneySchema,
  cash_after: MoneySchema,
  position_after: QuantitySchema,
  contributions: z.object({
    execution_spread_cost: FigureSchema,
    notional_rounding: FigureSchema,
    observed_midpoint_movement: FigureSchema,
  }),
  execution: z.object({
    ask: LevelSchema,
    bid: LevelSchema,
    executed_at: TimestampSchema,
    ingest_sequence: UnsignedIntegerSchema,
    midpoint: FigureSchema,
    quoted_spread: FigureSchema,
  }),
  signal: z.object({
    ask: DiagnosticDecimalSchema.nullable(),
    bid: DiagnosticDecimalSchema.nullable(),
    available_at: TimestampSchema,
    signal_at: TimestampSchema,
    class: ClassSchema,
    confidence_ppb: UnsignedIntegerSchema,
    ingest_sequence: UnsignedIntegerSchema,
    midpoint: FigureSchema,
    quoted_spread: FigureSchema,
  }),
  timing: z.object({
    configured_latency_ns: UnsignedIntegerSchema,
    signal_to_arrival_duration_ns: UnsignedIntegerSchema,
    arrival_to_execution_duration_ns: UnsignedIntegerSchema,
    signal_to_execution_duration_ns: UnsignedIntegerSchema,
  }),
});
export type FillRow = z.infer<typeof FillRowSchema>;

export const EpisodeRowSchema = z.object({
  ...LOCATION,
  episode: UnsignedIntegerSchema,
  state: z.enum(['completed', 'censored']),
  basis: z.enum(['booked', 'marked_at_final_valuation']),
  entry_at: TimestampSchema,
  exit_at: TimestampSchema.nullable(),
  censored_at: TimestampSchema.nullable(),
  duration_ns: UnsignedIntegerSchema,
  fills: UnsignedIntegerSchema,
  bought: DiagnosticDecimalSchema,
  sold: DiagnosticDecimalSchema,
  maximum_position: DiagnosticDecimalSchema,
  cash_flow: FigureSchema,
  execution_spread_cost: FigureSchema,
  fees: FigureSchema,
  pnl: FigureSchema,
});
export type EpisodeRow = z.infer<typeof EpisodeRowSchema>;

export const FillPageSchema = z.object({ fills: z.array(FillRowSchema), next_cursor: z.string().nullable() });
export type FillPage = z.infer<typeof FillPageSchema>;
export const EpisodePageSchema = z.object({ episodes: z.array(EpisodeRowSchema), next_cursor: z.string().nullable() });
export type EpisodePage = z.infer<typeof EpisodePageSchema>;

// --- Jobs -----------------------------------------------------------------------------------------------

export const DiagnosticsProgressSchema = z.object({
  stage: z.enum(['verify_sources', 'attribute', 'aggregate', 'publish']),
  fold: UnsignedIntegerSchema.optional(),
  folds: UnsignedIntegerSchema.optional(),
  model: ModelNameSchema.optional(),
  scenario: z.string().optional(),
});
export type DiagnosticsProgress = z.infer<typeof DiagnosticsProgressSchema>;

export const DiagnosticsJobSchema = z.object({
  job_id: JobIdSchema,
  kind: z.literal('strategy_diagnostics'),
  state: JobStateSchema,
  created_at: TimestampSchema,
  started_at: TimestampSchema.nullable(),
  finished_at: TimestampSchema.nullable(),
  request: z.object({ study_ids: z.array(ArtifactIdSchema) }),
  progress: DiagnosticsProgressSchema.nullable(),
  result: z
    .object({
      diagnostic_id: ArtifactIdSchema,
      outcome: z.enum(['published', 'already_present']),
      status: z.enum(['complete', 'partial']),
      study_ids: z.array(ArtifactIdSchema),
    })
    .nullable(),
  error: z.object({ code: z.string(), message: z.string(), reason: z.string().optional() }).nullable(),
  log: z.string(),
});
export type DiagnosticsJob = z.infer<typeof DiagnosticsJobSchema>;
export const DiagnosticsJobEnvelopeSchema = z.object({ job: DiagnosticsJobSchema });
export const StartedDiagnosticsSchema = z.object({ job: DiagnosticsJobSchema, deduplicated: z.boolean() });
export type StartedDiagnostics = z.infer<typeof StartedDiagnosticsSchema>;
