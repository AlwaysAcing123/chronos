// Runtime decoders for walk-forward evaluation (docs/research-service.md#walk-forward,
// ADR 0022): verified collections of independent recordings, frozen plans,
// and studies of every fold, model, and scenario. Every trading figure and
// difference is the service's exact decimal string; prediction quality is
// scored by the service. A missing figure is a reason, never a zero.
import { z } from 'zod';

import {
  ArtifactIdSchema,
  ConfigurationSchema,
  JobIdSchema,
  JobStateSchema,
  MetricSchema,
  ModelNameSchema,
} from './researchSchemas';
import { MoneySchema, PercentageSchema, TimestampSchema, UnsignedIntegerSchema } from './schemas';

export const EvidenceSchema = z.enum(['synthetic', 'observed']);
export type Evidence = z.infer<typeof EvidenceSchema>;

const ClassCountsSchema = z.object({ down: UnsignedIntegerSchema, flat: UnsignedIntegerSchema, up: UnsignedIntegerSchema });
const CountsSchema = z.record(z.string(), UnsignedIntegerSchema);
// A fold's evaluation window: the whole recording, end exclusive.
export const FoldWindowSchema = z.object({
  name: z.string().regex(/^fold_[1-9][0-9]*$/, 'expected a fold window'),
  start_at: TimestampSchema,
  end_at: TimestampSchema,
});

// --- Collections ----------------------------------------------------------------------------------------

const SessionSchema = z.union([
  z.object({ basis: z.literal('declared'), label: z.string() }),
  z.object({ unavailable: z.string() }),
]);

export const CollectionMemberSchema = z.object({
  dataset_id: ArtifactIdSchema,
  feature_artifact_id: ArtifactIdSchema,
  label_artifact_id: ArtifactIdSchema,
  stream: z.string(),
  events: UnsignedIntegerSchema,
  coverage: z.object({ completeness: z.literal('partial'), duration_ns: UnsignedIntegerSchema }),
  receive_time: z.object({ earliest: TimestampSchema, latest: TimestampSchema }),
  instruments: z.record(
    z.string(),
    z.object({
      feature_rows: UnsignedIntegerSchema,
      usable_labels: UnsignedIntegerSchema,
      first_available_at: TimestampSchema,
      last_available_at: TimestampSchema,
    }),
  ),
  provenance: z.object({
    evidence: EvidenceSchema,
    provider: z.string(),
    provider_dataset: z.string(),
    description: z.string(),
  }),
  session: SessionSchema,
});
export type CollectionMember = z.infer<typeof CollectionMemberSchema>;

export const CollectionSchema = z.object({
  collection_id: ArtifactIdSchema,
  evidence: EvidenceSchema,
  contract: z.object({ feature_set: z.string(), horizon_ns: UnsignedIntegerSchema, label_set: z.string() }),
  // How many members hold each instrument.
  instruments: CountsSchema,
  members: z.array(CollectionMemberSchema),
  definitions: z.record(z.string(), z.string()),
  verification: z.array(z.string()),
});
export type Collection = z.infer<typeof CollectionSchema>;

export const CollectionPageSchema = z.object({
  collections: z.array(CollectionSchema),
  invalid_entries: z.array(z.object({ collection_id: z.string(), reason: z.string() })),
  next_cursor: z.string().nullable(),
});
export type CollectionPage = z.infer<typeof CollectionPageSchema>;

// --- Requests and plans ---------------------------------------------------------------------------------

export const ScenarioSchema = z.object({
  name: z.string(),
  fee_per_fill: MoneySchema,
  latency_ns: UnsignedIntegerSchema,
});
export type Scenario = z.infer<typeof ScenarioSchema>;

export interface ScenarioDraft {
  readonly name: string;
  readonly fee_per_fill: string;
  readonly latency_ns: string;
}

export interface WalkForwardRequest {
  readonly collection_id: string;
  readonly instrument: string;
  readonly minimum_training_sources: string;
  readonly configuration: z.infer<typeof ConfigurationSchema>;
  readonly scenarios: readonly ScenarioDraft[];
}

const TrainingMemberSchema = z.object({
  dataset_id: ArtifactIdSchema,
  label_artifact_id: ArtifactIdSchema,
  stream: z.string(),
  feature_rows: UnsignedIntegerSchema,
  eligible_rows: UnsignedIntegerSchema,
  excluded: CountsSchema,
  interval: z.object({ start_at: TimestampSchema, end_at: TimestampSchema }),
});

export const PlanFoldSchema = z.object({
  index: UnsignedIntegerSchema,
  information_boundary: TimestampSchema,
  evaluation: z.object({
    dataset_id: ArtifactIdSchema,
    feature_artifact_id: ArtifactIdSchema,
    label_artifact_id: ArtifactIdSchema,
    stream: z.string(),
    feature_rows: UnsignedIntegerSchema,
    window: FoldWindowSchema,
  }),
  training: z.object({
    members: z.array(TrainingMemberSchema),
    eligible_rows: UnsignedIntegerSchema,
    class_support: ClassCountsSchema,
    max_prediction_at: TimestampSchema.nullable(),
    max_label_available_at: TimestampSchema.nullable(),
  }),
});
export type PlanFold = z.infer<typeof PlanFoldSchema>;

const ScoringRuleSchema = z.object({ cutoff: z.string(), rule: z.string(), unscored: z.string() });

export const PlanSchema = z.object({
  plan_id: ArtifactIdSchema,
  request_sha256: ArtifactIdSchema,
  collection: z.object({
    collection_id: ArtifactIdSchema,
    evidence: EvidenceSchema,
    skipped_members: z.array(z.object({ dataset_id: ArtifactIdSchema, reason: z.string() })),
  }),
  instrument: z.string(),
  horizon_ns: UnsignedIntegerSchema,
  configuration: ConfigurationSchema,
  request: z.object({ minimum_training_sources: UnsignedIntegerSchema }),
  scenarios: z.array(ScenarioSchema),
  scenario_rules: z.array(z.string()),
  scoring: ScoringRuleSchema,
  definitions: z.record(z.string(), z.string()),
  folds: z.array(PlanFoldSchema),
  workload: z.object({
    folds: UnsignedIntegerSchema,
    models: UnsignedIntegerSchema,
    scenarios: UnsignedIntegerSchema,
    schedules: UnsignedIntegerSchema,
    sources: UnsignedIntegerSchema,
    strategy_runs: UnsignedIntegerSchema,
    training_rows: UnsignedIntegerSchema,
    evaluation_rows: UnsignedIntegerSchema,
    caps: CountsSchema,
  }),
});
export type Plan = z.infer<typeof PlanSchema>;
export const PlanEnvelopeSchema = z.object({ plan: PlanSchema });

// --- Studies --------------------------------------------------------------------------------------------

const ValuationSchema = z.union([
  z.object({
    equity: MoneySchema,
    fees_paid: MoneySchema,
    net_total_pnl: MoneySchema,
    return_percent: PercentageSchema,
  }),
  z.object({ unavailable: z.object({ code: z.string(), message: z.string() }) }),
]);

const OrdersSchema = z.object({
  expired: UnsignedIntegerSchema,
  filled: UnsignedIntegerSchema,
  rejected: UnsignedIntegerSchema,
  scheduled: UnsignedIntegerSchema,
});

export const CellAccountSchema = z.object({
  valuation: ValuationSchema,
  drawdown: z.object({
    maximum: MoneySchema,
    maximum_percent_of_peak: PercentageSchema.nullable(),
    points_unvalued: UnsignedIntegerSchema,
  }),
  fills: UnsignedIntegerSchema,
  orders: OrdersSchema,
  turnover: z.object({ notional: MoneySchema, percent_of_starting_cash: PercentageSchema }),
});
export type CellAccount = z.infer<typeof CellAccountSchema>;

const DeltaSchema = z.union([
  z.object({ net_total_pnl: MoneySchema, return_percent: PercentageSchema }),
  z.object({ unavailable: z.string() }),
]);
export type Delta = z.infer<typeof DeltaSchema>;

const CELL = { scenario: z.string(), fee_per_fill: MoneySchema, latency_ns: UnsignedIntegerSchema };

export const CellSchema = z.discriminatedUnion('state', [
  z.object({
    ...CELL,
    state: z.literal('completed'),
    run: z.object({ artifact_id: ArtifactIdSchema, replay: z.string() }),
    starting_cash: MoneySchema,
    accounts: z.object({ strategy: CellAccountSchema, buy_and_hold: CellAccountSchema, cash: CellAccountSchema }),
    deltas: z.object({ buy_and_hold: DeltaSchema, cash: DeltaSchema }),
  }),
  z.object({
    ...CELL,
    state: z.literal('failed'),
    reason: z.string(),
    detail: z.string(),
    run: z.object({ artifact_id: ArtifactIdSchema }).optional(),
  }),
  z.object({ ...CELL, state: z.literal('unavailable'), reason: z.string() }),
]);
export type Cell = z.infer<typeof CellSchema>;

const ClassScoresSchema = z.object({ precision: MetricSchema, recall: MetricSchema, f1: MetricSchema });
export const ScoresSchema = z.object({
  metrics: z.object({
    accuracy: MetricSchema,
    balanced_accuracy: MetricSchema,
    log_loss: MetricSchema,
    brier_score: MetricSchema,
    rows: UnsignedIntegerSchema,
    per_class: z.object({ down: ClassScoresSchema, flat: ClassScoresSchema, up: ClassScoresSchema }),
    true_distribution: ClassCountsSchema,
    predicted_distribution: ClassCountsSchema,
    // Rows of each true class (outer) predicted as each class (inner).
    confusion_matrix: z.object({ down: ClassCountsSchema, flat: ClassCountsSchema, up: ClassCountsSchema }),
  }),
  rows: z.object({ scheduled: UnsignedIntegerSchema, scored: UnsignedIntegerSchema, not_scored: CountsSchema }),
});
export type Scores = z.infer<typeof ScoresSchema>;
const PredictiveSchema = z.union([ScoresSchema, z.object({ unavailable: z.string(), detail: z.string() })]);

export const StudyModelSchema = z.discriminatedUnion('state', [
  z.object({
    state: z.literal('completed'),
    schedule: z.object({
      artifact_id: ArtifactIdSchema,
      counts: z.object({ rows: UnsignedIntegerSchema, predicted: UnsignedIntegerSchema, classes: CountsSchema }),
    }),
    predictive: PredictiveSchema,
    cells: z.array(CellSchema),
  }),
  z.object({
    state: z.literal('unevaluable'),
    reason: z.string(),
    detail: z.record(z.string(), z.string()),
    cells: z.array(CellSchema),
  }),
  z.object({ state: z.literal('failed'), reason: z.string(), detail: z.string(), cells: z.array(CellSchema) }),
  z.object({ state: z.literal('unavailable'), reason: z.string(), cells: z.array(CellSchema) }),
]);
export type StudyModel = z.infer<typeof StudyModelSchema>;

export const StudyFoldSchema = z.object({
  index: UnsignedIntegerSchema,
  information_boundary: TimestampSchema,
  evaluation: z.object({
    dataset_id: ArtifactIdSchema,
    feature_rows: UnsignedIntegerSchema,
    stream: z.string(),
    window: FoldWindowSchema,
  }),
  fold_model: z.discriminatedUnion('state', [
    z.object({ state: z.literal('completed'), fold_model_id: ArtifactIdSchema }),
    z.object({ state: z.literal('failed'), reason: z.string(), detail: z.string() }),
  ]),
  training: z
    .object({
      rows: UnsignedIntegerSchema,
      class_support: ClassCountsSchema,
      max_prediction_at: TimestampSchema.nullable(),
      max_label_available_at: TimestampSchema.nullable(),
    })
    .optional(),
  models: z.object({ logistic: StudyModelSchema, majority: StudyModelSchema }),
});
export type StudyFold = z.infer<typeof StudyFoldSchema>;

const CoverageSchema = z.object({ included: z.array(UnsignedIntegerSchema), planned: UnsignedIntegerSchema });

const TotalsSchema = z.object({
  starting_capital: MoneySchema,
  accounts: z.record(
    z.enum(['strategy', 'buy_and_hold', 'cash']),
    z.object({
      net_total_pnl: MoneySchema,
      return_percent: PercentageSchema,
      fees_paid: MoneySchema,
      turnover: MoneySchema,
      fills: UnsignedIntegerSchema,
      orders: OrdersSchema,
    }),
  ),
  deltas: z.object({
    buy_and_hold: z.object({ net_total_pnl: MoneySchema, return_percent: PercentageSchema }),
    cash: z.object({ net_total_pnl: MoneySchema, return_percent: PercentageSchema }),
  }),
});
export type Totals = z.infer<typeof TotalsSchema>;

export const TradingAggregateSchema = z.object({
  folds: CoverageSchema,
  complete: z.union([
    TotalsSchema,
    z.object({
      unavailable: z.string(),
      missing: z.array(z.object({ fold: UnsignedIntegerSchema, reason: z.string() })),
    }),
  ]),
  partial: TotalsSchema.optional(),
});
export type TradingAggregate = z.infer<typeof TradingAggregateSchema>;

export const PredictiveAggregateSchema = z.union([
  z.object({ folds: CoverageSchema, status: z.enum(['complete', 'partial']), pooled: ScoresSchema }),
  z.object({ folds: CoverageSchema, status: z.literal('unavailable') }),
]);
export type PredictiveAggregate = z.infer<typeof PredictiveAggregateSchema>;

export const StudySchema = z.object({
  study_id: ArtifactIdSchema,
  plan_id: ArtifactIdSchema,
  collection: z.object({ collection_id: ArtifactIdSchema, evidence: EvidenceSchema }),
  instrument: z.string(),
  status: z.enum(['complete', 'partial']),
  scope: z.string(),
  models: z.array(ModelNameSchema),
  scenarios: z.array(ScenarioSchema),
  configuration: ConfigurationSchema,
  folds: z.array(StudyFoldSchema),
  missing: z.array(z.object({ fold: UnsignedIntegerSchema, item: z.string(), reason: z.string() })),
  outcomes: z.object({ cells: CountsSchema, models: CountsSchema }),
  aggregates: z.object({
    predictive: z.object({ logistic: PredictiveAggregateSchema, majority: PredictiveAggregateSchema }),
    trading: z.object({
      logistic: z.record(z.string(), TradingAggregateSchema),
      majority: z.record(z.string(), TradingAggregateSchema),
    }),
  }),
  definitions: z.object({
    aggregation: z.record(z.string(), z.string()),
    execution: z.array(z.string()),
    scoring: ScoringRuleSchema,
  }),
  environment: z.record(z.string(), z.string()),
});
export type Study = z.infer<typeof StudySchema>;

// Whether each fold model, schedule, and run still verifies, read again now.
export const StudyArtifactsSchema = z.array(
  z.object({
    fold: UnsignedIntegerSchema,
    fold_model: z.string().optional(),
    models: z.record(z.string(), z.object({ schedule: z.string().optional(), runs: z.record(z.string(), z.string()) })),
  }),
);
export type StudyArtifacts = z.infer<typeof StudyArtifactsSchema>;
export const StudyEnvelopeSchema = z.object({ study: StudySchema, artifacts: StudyArtifactsSchema });
export type StudyDetail = z.infer<typeof StudyEnvelopeSchema>;

export const StudyListingSchema = z.object({
  study_id: ArtifactIdSchema,
  plan_id: ArtifactIdSchema,
  collection_id: ArtifactIdSchema,
  evidence: EvidenceSchema,
  instrument: z.string(),
  folds: UnsignedIntegerSchema,
  missing: UnsignedIntegerSchema,
  status: z.enum(['complete', 'partial']),
});
export type StudyListing = z.infer<typeof StudyListingSchema>;
export const StudyPageSchema = z.object({
  studies: z.array(StudyListingSchema),
  invalid_entries: z.array(z.object({ study_id: z.string(), reason: z.string() })),
  next_cursor: z.string().nullable(),
});
export type StudyPage = z.infer<typeof StudyPageSchema>;

// --- Jobs -----------------------------------------------------------------------------------------------

const JOB = {
  job_id: JobIdSchema,
  state: JobStateSchema,
  created_at: TimestampSchema,
  started_at: TimestampSchema.nullable(),
  finished_at: TimestampSchema.nullable(),
  error: z.object({ code: z.string(), message: z.string(), reason: z.string().optional() }).nullable(),
  log: z.string(),
};

// The latest stage a running study reported.
export const ProgressSchema = z.object({
  stage: z.enum(['verify_sources', 'fold', 'aggregate', 'publish']),
  fold: UnsignedIntegerSchema.optional(),
  folds: UnsignedIntegerSchema.optional(),
  step: z.enum(['fit', 'schedule', 'run', 'score']).optional(),
  model: ModelNameSchema.optional(),
  scenario: z.string().optional(),
});
export type Progress = z.infer<typeof ProgressSchema>;

export const PlanJobSchema = z.object({
  ...JOB,
  kind: z.literal('walk_forward_plan'),
  request: z.object({ collection_id: ArtifactIdSchema, instrument: z.string() }),
  result: z
    .object({ plan_id: ArtifactIdSchema, folds: UnsignedIntegerSchema, outcome: z.enum(['published', 'already_present']) })
    .nullable(),
});
export type PlanJob = z.infer<typeof PlanJobSchema>;

export const StudyJobSchema = z.object({
  ...JOB,
  kind: z.literal('walk_forward'),
  request: z.object({ plan_id: ArtifactIdSchema }),
  progress: ProgressSchema.nullable(),
  result: z
    .object({
      study_id: ArtifactIdSchema,
      status: z.enum(['complete', 'partial']),
      missing: UnsignedIntegerSchema,
      outcome: z.enum(['published', 'already_present']),
    })
    .nullable(),
});
export type StudyJob = z.infer<typeof StudyJobSchema>;

export const WalkForwardJobSchema = z.discriminatedUnion('kind', [PlanJobSchema, StudyJobSchema]);
export type WalkForwardJob = z.infer<typeof WalkForwardJobSchema>;
export const WalkForwardJobEnvelopeSchema = z.object({ job: WalkForwardJobSchema });
export const StartedPlanSchema = z.object({ job: PlanJobSchema, deduplicated: z.boolean() });
export type StartedPlan = z.infer<typeof StartedPlanSchema>;
export const StartedStudySchema = z.object({ job: StudyJobSchema, deduplicated: z.boolean() });
export type StartedStudy = z.infer<typeof StartedStudySchema>;
export const StudyJobPageSchema = z.object({ jobs: z.array(StudyJobSchema), next_cursor: z.string().nullable() });
export type StudyJobPage = z.infer<typeof StudyJobPageSchema>;
