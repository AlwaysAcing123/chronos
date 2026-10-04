// Model deployments on the paper-trading API (docs/model-deployments.md):
// a verified research model trading its own paper account, only after an
// explicit Start. Every response is runtime-decoded, as everywhere else.
import { z } from 'zod';

import { isCanonicalDecimal } from './decimal';
import { ChronosApiError } from './errors';
import type { HttpResponse, TransportOptions } from './http';
import { decodeBody, failureOf, sendRequest } from './http';
import { DEFAULT_TRANSPORT } from './client';
import {
  IdentifierSchema,
  MoneySchema,
  QuantitySchema,
  SideSchema,
  TimestampSchema,
  UnsignedIntegerSchema,
} from './schemas';

const RatioSchema = z.string().refine((text) => isCanonicalDecimal(text, 9), {
  message: 'expected a canonical decimal string with at most 9 fractional digits',
});
const Sha256Schema = z.string().regex(/^[0-9a-f]{64}$/, 'expected a SHA-256 digest');

export const DeploymentStateSchema = z.enum(['stopped', 'running', 'stopping', 'faulted']);
export type DeploymentState = z.infer<typeof DeploymentStateSchema>;

const CountsSchema = z.record(z.string(), UnsignedIntegerSchema);

export const RunCountersSchema = z.object({
  candidates: UnsignedIntegerSchema,
  superseded: UnsignedIntegerSchema,
  skipped: CountsSchema,
  predictions: UnsignedIntegerSchema,
  holds: UnsignedIntegerSchema,
  order_decisions: UnsignedIntegerSchema,
  orders_recorded: UnsignedIntegerSchema,
  abandoned: UnsignedIntegerSchema,
  waits: CountsSchema,
  last_inference_us: UnsignedIntegerSchema,
  max_inference_us: UnsignedIntegerSchema,
  last_submission_us: UnsignedIntegerSchema,
  max_submission_us: UnsignedIntegerSchema,
  last_error: z.string().nullable(),
});
export type RunCounters = z.infer<typeof RunCountersSchema>;

export const DeploymentSchema = z.object({
  trading_mode: z.literal('paper'),
  deployment_id: IdentifierSchema,
  account_id: IdentifierSchema,
  creation_sequence: UnsignedIntegerSchema,
  state: DeploymentStateSchema,
  state_reason: z.string().min(1),
  state_changed_at: TimestampSchema,
  created_at: TimestampSchema,
  active: z.boolean(),
  run_generation: UnsignedIntegerSchema,
  stream_id: z.string().min(1),
  starting_cash: MoneySchema,
  model: z.object({
    experiment_id: Sha256Schema,
    name: z.enum(['logistic', 'majority']),
    instrument: IdentifierSchema,
    parameters_sha256: Sha256Schema,
    feature_set: z.string().min(1),
    training_cutoff: TimestampSchema,
  }),
  policy: z.object({
    name: z.string().min(1),
    threshold: RatioSchema,
    target_quantity: QuantitySchema,
  }),
  timing: z.object({
    decision_interval_ms: UnsignedIntegerSchema,
    latency_ms: UnsignedIntegerSchema,
    max_signal_age_ms: UnsignedIntegerSchema,
  }),
  limits: z.object({
    max_orders_per_run: UnsignedIntegerSchema,
    max_consecutive_rejections: UnsignedIntegerSchema,
    orders_this_run: UnsignedIntegerSchema,
    consecutive_rejections: UnsignedIntegerSchema,
  }),
  watermark: z.object({ received_at: TimestampSchema, ingest_sequence: UnsignedIntegerSchema }).nullable(),
  next_decision_sequence: UnsignedIntegerSchema,
  pending_decision: UnsignedIntegerSchema.nullable(),
  version: UnsignedIntegerSchema,
  run: RunCountersSchema.nullable(),
});
export type Deployment = z.infer<typeof DeploymentSchema>;

export const DecisionSchema = z.object({
  decision_sequence: UnsignedIntegerSchema,
  run_generation: UnsignedIntegerSchema,
  state: z.enum(['held', 'pending', 'recorded', 'abandoned']),
  observation: z.object({
    stream_id: z.string().min(1),
    instrument: IdentifierSchema,
    ingest_sequence: UnsignedIntegerSchema,
    event_at: TimestampSchema,
    received_at: TimestampSchema,
  }),
  decided_at: TimestampSchema,
  inputs: z.array(z.string().regex(/^-?(0|[1-9][0-9]*)$/)).length(5),
  prediction: z.object({
    class: z.enum(['down', 'flat', 'up']),
    probabilities: z.array(z.string()).length(3),
    confidence: RatioSchema,
  }),
  position_before: QuantitySchema,
  target_quantity: QuantitySchema.nullable(),
  policy_reason: z.string().min(1),
  order: z
    .object({
      client_order_id: IdentifierSchema,
      side: SideSchema,
      quantity: QuantitySchema,
      not_before_received_at: TimestampSchema.nullable(),
      expires_at: TimestampSchema.nullable(),
    })
    .nullable(),
  resolution: z.string().nullable(),
  execution: z.object({ ingest_sequence: UnsignedIntegerSchema, received_at: TimestampSchema.nullable() }).nullable(),
  resolved_at: TimestampSchema.nullable(),
  created_at: TimestampSchema,
});
export type Decision = z.infer<typeof DecisionSchema>;

const DeploymentEnvelopeSchema = z.object({ deployment: DeploymentSchema });
const CreatedSchema = z.object({ deployment: DeploymentSchema, created: z.boolean() });
const DeploymentPageSchema = z.object({
  enabled: z.boolean(),
  deployments: z.array(DeploymentSchema),
  next_cursor: z.string().nullable(),
});
export type DeploymentPage = z.infer<typeof DeploymentPageSchema>;
const DecisionPageSchema = z.object({ decisions: z.array(DecisionSchema), next_cursor: z.string().nullable() });
export type DecisionPage = z.infer<typeof DecisionPageSchema>;
const KillSwitchSchema = z.object({ stopped: z.array(DeploymentSchema) });

// Exactly what the server is asked to create; reviewed before it is sent.
export interface DeploymentRequest {
  readonly deployment_id: string;
  readonly account_id: string;
  readonly starting_cash: string;
  readonly experiment_id: string;
  readonly model: 'logistic' | 'majority';
  readonly instrument: string;
  readonly threshold: string;
  readonly target_quantity: string;
  readonly decision_interval_ms: string;
  readonly latency_ms: string;
  readonly max_signal_age_ms: string;
  readonly max_orders_per_run: string;
  readonly max_consecutive_rejections: string;
}

export interface DeploymentClient {
  listDeployments(cursor: string | null, signal?: AbortSignal): Promise<DeploymentPage>;
  getDeployment(deploymentId: string, signal?: AbortSignal): Promise<Deployment>;
  createDeployment(request: DeploymentRequest): Promise<{ deployment: Deployment; created: boolean }>;
  startDeployment(deploymentId: string): Promise<Deployment>;
  stopDeployment(deploymentId: string): Promise<Deployment>;
  killSwitch(): Promise<readonly Deployment[]>;
  listDecisions(deploymentId: string, cursor: string | null, signal?: AbortSignal): Promise<DecisionPage>;
}

const PAGE_SIZE = 50;

// Only documented pre-action refusals prove a control was not accepted.
// A timeout, a bad response, or persistence failure can follow its commit.
const CONTROL_REFUSALS: ReadonlySet<string> = new Set([
  'malformed_request', 'invalid_deployment', 'deployment_not_found',
  'deployment_exists', 'account_exists', 'deployments_disabled',
  'invalid_deployment_state', 'model_unavailable', 'unsupported_media_type',
  'payload_too_large', 'method_not_allowed', 'route_not_found',
]);

export function isUncertainDeploymentControl(error: unknown): boolean {
  if (error == null) return false;
  return !(error instanceof ChronosApiError && error.failure.kind === 'api_error' &&
    error.failure.status < 500 && CONTROL_REFUSALS.has(error.failure.error.code));
}

function require(response: HttpResponse, ...statuses: number[]): HttpResponse {
  if (!statuses.includes(response.status)) {
    throw failureOf(response);
  }
  return response;
}

function deploymentPath(deploymentId: string): string {
  return `/api/v1/deployments/${encodeURIComponent(deploymentId)}`;
}

function pagePath(path: string, cursor: string | null): string {
  const query = new URLSearchParams({ limit: String(PAGE_SIZE) });
  if (cursor !== null) {
    query.set('cursor', cursor);
  }
  return `${path}?${query.toString()}`;
}

export function createDeploymentClient(transport: TransportOptions = DEFAULT_TRANSPORT): DeploymentClient {
  const get = (path: string, signal?: AbortSignal) => sendRequest(transport, { method: 'GET', path, signal });
  const post = (path: string, body: unknown) => sendRequest(transport, { method: 'POST', path, body });
  return {
    async listDeployments(cursor, signal) {
      return decodeBody(DeploymentPageSchema, require(await get(pagePath('/api/v1/deployments', cursor), signal), 200));
    },
    async getDeployment(deploymentId, signal) {
      return decodeBody(DeploymentEnvelopeSchema, require(await get(deploymentPath(deploymentId), signal), 200))
        .deployment;
    },
    async createDeployment(request) {
      return decodeBody(CreatedSchema, require(await post('/api/v1/deployments', request), 200, 201));
    },
    async startDeployment(deploymentId) {
      return decodeBody(DeploymentEnvelopeSchema, require(await post(`${deploymentPath(deploymentId)}/start`, {}), 200))
        .deployment;
    },
    async stopDeployment(deploymentId) {
      return decodeBody(DeploymentEnvelopeSchema, require(await post(`${deploymentPath(deploymentId)}/stop`, {}), 200))
        .deployment;
    },
    async killSwitch() {
      return decodeBody(KillSwitchSchema, require(await post('/api/v1/deployments/kill-switch', {}), 200)).stopped;
    },
    async listDecisions(deploymentId, cursor, signal) {
      return decodeBody(
        DecisionPageSchema,
        require(await get(pagePath(`${deploymentPath(deploymentId)}/decisions`, cursor), signal), 200),
      );
    },
  };
}
