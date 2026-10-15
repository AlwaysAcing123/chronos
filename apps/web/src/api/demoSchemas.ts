// The demo workspace's guide data (ADR 0025): GET /api/v1/research/demo,
// served only by a demo workspace's research service. It is navigation
// metadata: the verified seed's artifact IDs, routes, chronology, and audit
// verdicts, and the session's number and mode. It carries no metric or
// money; every figure the guide points to is read from its own view.
import { z } from 'zod';

import { TimestampSchema } from './schemas';

const Id = z.string().regex(/^[0-9a-f]{64}$/, 'expected a 64-digit lowercase ID');
const Timestamp = TimestampSchema;
const Counter = z.string().regex(/^(0|[1-9][0-9]*)$/, 'expected a whole number');
// A route inside this application, never another origin or a file.
const HashRoute = z.string().regex(/^#\/[a-z0-9/_-]*$/, 'expected an application route');
const Unavailable = z.object({ unavailable: z.string().min(1) });

const AuditSchema = z.object({ checks_total: Counter, verdict: z.enum(['pass', 'fail']) });

export const DemoSessionSchema = z.object({
  number: Counter,
  // playback: a fresh session with a replay; history: a retained one, without.
  mode: z.enum(['playback', 'history']),
});
export type DemoSession = z.infer<typeof DemoSessionSchema>;

const SessionSchema = z.object({
  name: z.string().min(1),
  role: z.enum(['history', 'playback']),
  day: z.string().min(1),
  declared_label: z.string().min(1),
  stream: z.string().min(1),
  dataset_id: Id,
  events: Counter,
  receive_first: Timestamp,
  receive_last: Timestamp,
});

export const DemoSeedSchema = z.object({
  schema: z.object({ name: z.literal('chronos.demo_seed'), version: z.literal('1') }),
  seed_id: Id,
  notice: z.string().min(1),
  scenario: z.object({
    name: z.string().min(1),
    version: z.string().min(1),
    evidence: z.literal('synthetic'),
    instruments: z.array(z.string().min(1)).min(1),
    sessions: z.array(SessionSchema).min(1),
  }),
  playback: z.object({
    dataset_id: Id,
    stream: z.string().min(1),
    events: Counter,
    instruments: z.array(z.string().min(1)).min(1),
    receive_first: Timestamp,
    receive_last: Timestamp,
  }),
  chronology: z.object({
    checks: z.array(z.string().min(1)).min(1),
    history_ends: Timestamp,
    playback_starts: Timestamp,
    model_cutoffs: z.array(z.object({ family: z.string().min(1), at: Timestamp, model_version_id: Id })),
  }),
  audits: z.object({
    walk_forward: z.record(z.string(), AuditSchema),
    strategy_diagnostics: AuditSchema,
  }),
  artifacts: z.object({
    model_versions: z.object({ logistic: Id, majority: Id }),
    comparison_id: Id,
    walk_forward_studies: z.record(z.string(), Id),
    strategy_diagnostics: z.object({ diagnostic_id: Id }),
  }),
  routes: z.object({
    trading: HashRoute,
    research: HashRoute,
    walk_forward: HashRoute,
    models: z.object({ logistic: HashRoute, majority: HashRoute }),
    comparison: HashRoute,
    studies: z.record(z.string(), HashRoute),
    diagnostics: HashRoute,
    diagnostics_run: z.union([HashRoute, Unavailable]),
  }),
});
export type DemoSeed = z.infer<typeof DemoSeedSchema>;

export const DemoGuideSchema = z.object({ seed: DemoSeedSchema, session: DemoSessionSchema });
export type DemoGuide = z.infer<typeof DemoGuideSchema>;
