// Runtime decoders for comparisons of registered model versions
// (docs/research-service.md#comparisons). Every trading metric is the
// engine's exact decimal string and every delta the service's; prediction
// quality is scored by the service against realized labels. A missing figure
// is a reason, never a zero.
import { z } from 'zod';

import {
  ArtifactIdSchema,
  ConfigurationSchema,
  JobIdSchema,
  JobStateSchema,
  MetricSchema,
  WindowNameSchema,
} from './researchSchemas';
import { MoneySchema, PercentageSchema, TimestampSchema, UnsignedIntegerSchema } from './schemas';

export const ComparisonRequestSchema = z.object({
  model_version_ids: z.array(ArtifactIdSchema),
  split_artifact_id: ArtifactIdSchema,
  instrument: z.string(),
  window: WindowNameSchema,
  configuration: ConfigurationSchema,
});
export type ComparisonRequest = z.infer<typeof ComparisonRequestSchema>;

const StatusCountsSchema = z.object({
  completed: UnsignedIntegerSchema,
  failed: UnsignedIntegerSchema,
  refused: UnsignedIntegerSchema,
});

export const ComparisonJobSchema = z.object({
  job_id: JobIdSchema,
  kind: z.literal('comparison'),
  state: JobStateSchema,
  created_at: TimestampSchema,
  started_at: TimestampSchema.nullable(),
  finished_at: TimestampSchema.nullable(),
  request: ComparisonRequestSchema,
  result: z
    .object({
      comparison_id: ArtifactIdSchema,
      outcome: z.enum(['published', 'already_present']),
      status: StatusCountsSchema,
    })
    .nullable(),
  error: z.object({ code: z.string(), message: z.string(), reason: z.string().optional() }).nullable(),
  log: z.string(),
});
export type ComparisonJob = z.infer<typeof ComparisonJobSchema>;
export const StartedComparisonSchema = z.object({ job: ComparisonJobSchema, deduplicated: z.boolean() });
export type StartedComparison = z.infer<typeof StartedComparisonSchema>;

const ValuationSchema = z.union([
  z.object({
    equity: MoneySchema,
    fees_paid: MoneySchema,
    net_total_pnl: MoneySchema,
    return_percent: PercentageSchema,
  }),
  z.object({ unavailable: z.string() }),
]);
export type ComparisonValuation = z.infer<typeof ValuationSchema>;

export const AccountMetricsSchema = z.object({
  valuation: ValuationSchema,
  drawdown: z.object({
    maximum: MoneySchema,
    maximum_percent_of_peak: PercentageSchema.nullable(),
    points_unvalued: UnsignedIntegerSchema,
  }),
  fills: UnsignedIntegerSchema,
  orders: z.object({
    expired: UnsignedIntegerSchema,
    filled: UnsignedIntegerSchema,
    rejected: UnsignedIntegerSchema,
    scheduled: UnsignedIntegerSchema,
  }),
  turnover: z.object({ notional: MoneySchema, percent_of_starting_cash: PercentageSchema }),
});
export type AccountMetrics = z.infer<typeof AccountMetricsSchema>;

// A difference the service computed exactly, or why it could not.
const Unavailable = z.object({ unavailable: z.string() });
const DeltaPairSchema = z.object({
  net_total_pnl: z.union([z.object({ value: MoneySchema }), Unavailable]),
  return_percent: z.union([z.object({ value: PercentageSchema }), Unavailable]),
});
export type DeltaPair = z.infer<typeof DeltaPairSchema>;

// A completed member's predictions scored against the window's realized
// labels, or why they could not be.
const ClassScoresSchema = z.object({ precision: MetricSchema, recall: MetricSchema, f1: MetricSchema });
const PredictiveSchema = z.union([
  z.object({
    metrics: z.object({
      accuracy: MetricSchema,
      balanced_accuracy: MetricSchema,
      log_loss: MetricSchema,
      brier_score: MetricSchema,
      rows: UnsignedIntegerSchema,
      per_class: z.object({ down: ClassScoresSchema, flat: ClassScoresSchema, up: ClassScoresSchema }),
    }),
    rows: z.object({
      scheduled: UnsignedIntegerSchema,
      scored: UnsignedIntegerSchema,
      not_scored: z.record(z.string(), UnsignedIntegerSchema),
    }),
  }),
  z.object({ unavailable: z.string(), detail: z.string() }),
]);
export type Predictive = z.infer<typeof PredictiveSchema>;

export const MemberStateSchema = z.enum(['completed', 'refused', 'failed']);
export type MemberState = z.infer<typeof MemberStateSchema>;

const CutoffSchema = z.object({
  basis: z.enum(['verified', 'declared', 'unavailable']),
  at: TimestampSchema.optional(),
  reason: z.string().optional(),
});

export const MemberSchema = z.object({
  model_version_id: ArtifactIdSchema,
  names: z.array(z.object({ name: z.string(), version: z.string() })),
  state: MemberStateSchema,
  reason: z.string().optional(),
  detail: z.string().optional(),
  family: z.string().optional(),
  origin: z.enum(['experiment', 'import']).optional(),
  information_cutoff: CutoffSchema.optional(),
  conditional: z.boolean().optional(),
  schedule: z
    .object({
      artifact_id: ArtifactIdSchema,
      reproduced: z.boolean().optional(),
      counts: z.object({
        rows: UnsignedIntegerSchema,
        predicted: UnsignedIntegerSchema,
        classes: z.record(z.string(), UnsignedIntegerSchema),
      }),
    })
    .optional(),
  run: z.object({ artifact_id: ArtifactIdSchema, replayed: z.boolean(), replay: z.string().optional() }).optional(),
  accounts: z
    .object({ strategy: AccountMetricsSchema, buy_and_hold: AccountMetricsSchema, cash: AccountMetricsSchema })
    .optional(),
  deltas: z
    .object({ buy_and_hold: DeltaPairSchema, cash: DeltaPairSchema, reference: DeltaPairSchema.optional() })
    .optional(),
  predictive: PredictiveSchema.optional(),
});
export type ComparisonMember = z.infer<typeof MemberSchema>;

const WindowSchema = z.object({ name: WindowNameSchema, start_at: TimestampSchema, end_at: TimestampSchema.nullable() });

export const ComparisonSchema = z.object({
  comparison_id: ArtifactIdSchema,
  spec: ComparisonRequestSchema,
  spec_sha256: ArtifactIdSchema,
  sources: z.object({ dataset_id: ArtifactIdSchema, split_artifact_id: ArtifactIdSchema, stream: z.string() }),
  window: WindowSchema,
  members: z.array(MemberSchema),
  reference_model_version_id: ArtifactIdSchema.nullable(),
  benchmarks: z.union([
    z.object({
      accounts: z.object({ buy_and_hold: AccountMetricsSchema, cash: AccountMetricsSchema }),
      identical_across_members: z.boolean(),
      members_differing: z.array(ArtifactIdSchema),
    }),
    z.object({ unavailable: z.string() }),
  ]),
  fairness: z.array(z.string()),
  scoring: z.array(z.string()),
  status: StatusCountsSchema,
  environment: z.record(z.string(), z.string()),
});
export type Comparison = z.infer<typeof ComparisonSchema>;

// Whether each member's schedule and run still verify, read again now.
export const ArtifactCheckSchema = z.object({
  model_version_id: ArtifactIdSchema,
  schedule: z.string().optional(),
  run: z.string().optional(),
});
export type ArtifactCheck = z.infer<typeof ArtifactCheckSchema>;

export const ComparisonEnvelopeSchema = z.object({
  comparison: ComparisonSchema,
  artifacts: z.array(ArtifactCheckSchema),
});
export type ComparisonDetail = z.infer<typeof ComparisonEnvelopeSchema>;

export const ComparisonListingSchema = z.object({
  comparison_id: ArtifactIdSchema,
  instrument: z.string(),
  members: z.array(MemberSchema.pick({ model_version_id: true, names: true, state: true })),
  status: StatusCountsSchema,
  window: WindowSchema,
});
export type ComparisonListing = z.infer<typeof ComparisonListingSchema>;

export const ComparisonPageSchema = z.object({
  comparisons: z.array(ComparisonListingSchema),
  invalid_entries: z.array(z.object({ comparison_id: z.string(), reason: z.string() })),
  next_cursor: z.string().nullable(),
});
export type ComparisonPage = z.infer<typeof ComparisonPageSchema>;
