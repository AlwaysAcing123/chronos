import { useId, useState } from 'react';

import type { Deployment, DeploymentSettings, ExperimentDeploymentRequest } from '../../api/deployments';
import { isUncertainDeploymentControl } from '../../api/deployments';
import { ChronosApiError } from '../../api/errors';
import { isValidIdentifier } from '../../api/identifiers';
import type { ExperimentPage, ModelName, RunConfiguration } from '../../api/researchSchemas';
import { MODEL_LABEL, ppbAsPercent } from '../../presentation/research';
import { useCreateDeployment, useDeploymentClient } from '../../state/deployments';
import { Alert, Code } from '../ui/Alert';
import { Button } from '../ui/Button';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import type { Selection } from './ModelCatalog';
import styles from './research.module.css';

// What every deployment does, whatever its configuration (ADR 0018).
export const DEPLOYMENT_RULES = [
  'A new paper account is created for the deployment and belongs to it for good: manual orders and cancellations on it are refused.',
  'Nothing trades until you press Start, and nothing resumes after the server restarts.',
  'No decision is made on market data received before the model’s training cutoff, or on an observation already decided on.',
  'Long or flat, never short, and never more than the target position, which is at most one share; limits are enforced by the server.',
  'Each order is a market immediate-or-cancel order on the current quote, at least the latency after its signal and never on the signal’s own quote; a signal older than its maximum age is abandoned.',
  'Stop and the kill switch end trading and keep the position; nothing is liquidated.',
] as const;

export type Draft = Omit<DeploymentSettings, 'instrument'>;

interface FieldSpec {
  readonly key: keyof Draft;
  readonly label: string;
  readonly hint?: (value: string) => string | null;
}

export const FIELDS: readonly FieldSpec[] = [
  { key: 'deployment_id', label: 'Deployment ID', hint: () => 'Up to 32 letters, digits, dots, dashes, or underscores' },
  { key: 'account_id', label: 'New paper account ID', hint: () => 'Must not exist yet' },
  { key: 'starting_cash', label: 'Starting cash (USD)' },
  {
    key: 'threshold',
    label: 'Confidence threshold (0 to 1)',
    hint: (value) => {
      const ppb = ratioAsPpb(value);
      return ppb === null ? null : `The predicted class needs at least ${ppbAsPercent(ppb)}%`;
    },
  },
  { key: 'target_quantity', label: 'Target position (shares, at most 1)' },
  { key: 'decision_interval_ms', label: 'Minimum time between decisions (ms)' },
  { key: 'latency_ms', label: 'Signal-to-order latency (ms)' },
  { key: 'max_signal_age_ms', label: 'Maximum signal age (ms)' },
  { key: 'max_orders_per_run', label: 'Orders per run before it faults' },
  { key: 'max_consecutive_rejections', label: 'Consecutive rejections before it faults' },
];

// Exact conversions between a decimal ratio and parts per billion, so no
// binary floating point touches a threshold.
export function ratioAsPpb(ratio: string): string | null {
  const match = /^(0|1)(?:\.(\d{1,9}))?$/.exec(ratio);
  if (match === null) return null;
  const ppb = `${match[1] ?? '0'}${(match[2] ?? '').padEnd(9, '0')}`.replace(/^0+(?=\d)/, '');
  return ppb.length > 10 || (ppb.length === 10 && ppb !== '1000000000') ? null : ppb;
}

export function ppbAsRatio(ppb: string): string {
  const digits = ppb.padStart(10, '0');
  const whole = digits.slice(0, -9).replace(/^0+(?=\d)/, '');
  const fraction = digits.slice(-9).replace(/0+$/, '');
  return fraction === '' ? whole : `${whole}.${fraction}`;
}

function millisecondsOf(nanoseconds: string, fallback: string): string {
  return /^\d+$/.test(nanoseconds) && nanoseconds.endsWith('000000') && nanoseconds !== '0'
    ? nanoseconds.slice(0, -6)
    : fallback;
}

export function initialDraft(defaults: RunConfiguration | undefined): Draft {
  return {
    deployment_id: '',
    account_id: '',
    starting_cash: defaults?.starting_cash ?? '10000',
    threshold: defaults ? ppbAsRatio(defaults.threshold_ppb) : '0.55',
    target_quantity: '1',
    decision_interval_ms: millisecondsOf(defaults?.decision_interval_ns ?? '', '1000'),
    latency_ms: millisecondsOf(defaults?.latency_ns ?? '', '100'),
    max_signal_age_ms: '5000',
    max_orders_per_run: '20',
    max_consecutive_rejections: '3',
  };
}

export function rejectedField(error: unknown): string | null {
  return error instanceof ChronosApiError && error.failure.kind === 'api_error' ? error.failure.error.field : null;
}

// What became of a creation request: refused, uncertain, or created stopped.
export function DeploymentCreationNotice({
  error,
  created,
}: {
  readonly error: Error | null;
  readonly created: { readonly deployment: Deployment; readonly created: boolean } | undefined;
}) {
  return (
    <>
      {error !== null && (
        <div className={styles.section}>
          <Alert
            tone={isUncertainDeploymentControl(error) ? 'caution' : 'negative'}
            title={isUncertainDeploymentControl(error) ? 'Creation outcome unknown' : 'Creation request rejected'}
            live
          >
            {isUncertainDeploymentControl(error) && (
              <p>
                The server may have created the deployment. Check Paper deployments or retry this same reviewed request
                with the same IDs; nothing trades until Start is explicitly requested.
              </p>
            )}
            {error instanceof ChronosApiError && error.failure.kind === 'api_error' ? (
              <p>
                {error.failure.error.message} <Code>{error.failure.error.reason ?? error.failure.error.code}</Code>
              </p>
            ) : (
              <p>{error.message}</p>
            )}
          </Alert>
        </div>
      )}
      {created !== undefined && (
        <div className={styles.section}>
          <Alert
            tone={created.created ? 'positive' : 'info'}
            title={created.created ? 'Deployment created, stopped' : 'This deployment already exists'}
            live
          >
            <p>
              {created.deployment.deployment_id} trades account {created.deployment.account_id} only after you start it
              under Paper deployments. The server verified model parameters{' '}
              <Code>{created.deployment.model.parameters_sha256.slice(0, 12)}</Code>.
            </p>
          </Alert>
        </div>
      )}
    </>
  );
}

// Creates a deployment, stopped, from the selected model. The request is
// reviewed and frozen before it is sent; the server verifies the model.
export function DeploymentForm({
  selection,
  experiments,
  defaults,
}: {
  readonly selection: Selection | null;
  readonly experiments: ExperimentPage | undefined;
  readonly defaults: RunConfiguration | undefined;
}) {
  const formId = useId();
  const [model, setModel] = useState<ModelName>('logistic');
  const [draft, setDraft] = useState<Draft>(() => initialDraft(defaults));
  const [review, setReview] = useState<ExperimentDeploymentRequest | null>(null);
  const create = useCreateDeployment();
  const client = useDeploymentClient();

  if (client === null) {
    return null;
  }
  const experiment = experiments?.experiments.find((entry) => entry.experiment_id === selection?.experimentId);
  const entry = experiment?.instruments.find((item) => item.instrument === selection?.instrument);
  if (selection === null || experiment === undefined || entry === undefined) {
    return (
      <Panel title="Deploy to paper">
        <p className={styles.note}>Choose a model with a fitted instrument to deploy it to a paper account.</p>
      </Panel>
    );
  }
  const fitted = entry.models[model].status === 'fitted';
  const blank = FIELDS.find((field) => draft[field.key].trim() === '');
  const badIdentifier =
    !isValidIdentifier(draft.deployment_id) || draft.deployment_id.length > 32 || !isValidIdentifier(draft.account_id);
  const rejected = rejectedField(create.error);
  const request: ExperimentDeploymentRequest = {
    ...draft,
    experiment_id: experiment.experiment_id,
    instrument: entry.instrument,
    model,
  };

  return (
    <Panel title="Deploy to paper" meta={`${review?.instrument ?? entry.instrument} · ${MODEL_LABEL[review?.model ?? model]}`}>
      <form
        id={formId}
        onSubmit={(event) => {
          event.preventDefault();
          if (create.isPending) return;
          if (review === null) {
            if (!fitted || blank !== undefined || badIdentifier) return;
            // Exactly what was reviewed is sent, whatever changes meanwhile.
            setReview(request);
            return;
          }
          create.mutate(review, {
            onSuccess: () => {
              setReview(null);
            },
          });
        }}
      >
        {review === null ? (
          <>
            <div className={styles.section}>
              <div className={styles.field}>
                <label htmlFor={`${formId}-model`}>Model</label>
                <select
                  id={`${formId}-model`}
                  value={model}
                  onChange={(event) => {
                    setModel(event.target.value as ModelName);
                  }}
                >
                  {(['logistic', 'majority'] as const).map((name) => (
                    <option key={name} value={name} disabled={entry.models[name].status !== 'fitted'}>
                      {MODEL_LABEL[name]}
                      {entry.models[name].status === 'fitted' ? '' : ' (unevaluable)'}
                    </option>
                  ))}
                </select>
              </div>
              <p className={styles.note}>
                Trained on data before <MarketTime value={experiment.windows.validation.start_at} />; it decides only
                on later market data.
              </p>
            </div>
            <div className={styles.section}>
              <h3 className={styles.sectionTitle}>Account, policy, timing, and limits</h3>
              <div className={styles.fields}>
                {FIELDS.map((field) => {
                  const id = `${formId}-${field.key}`;
                  const hint = field.hint?.(draft[field.key]) ?? null;
                  return (
                    <div key={field.key} className={styles.field}>
                      <label htmlFor={id}>{field.label}</label>
                      <input
                        id={id}
                        autoComplete="off"
                        spellCheck={false}
                        inputMode={field.key.endsWith('_id') ? 'text' : 'decimal'}
                        value={draft[field.key]}
                        aria-invalid={rejected === field.key}
                        aria-describedby={hint === null ? undefined : `${id}-hint`}
                        onChange={(event) => {
                          setDraft({ ...draft, [field.key]: event.target.value });
                        }}
                      />
                      {hint !== null && (
                        <span id={`${id}-hint`} className={styles.fieldHint}>
                          {hint}
                        </span>
                      )}
                    </div>
                  );
                })}
              </div>
            </div>
          </>
        ) : (
          <div className={styles.review} aria-label="Review the deployment">
            <Facts
              facts={[
                { label: 'Instrument', value: review.instrument },
                { label: 'Model', value: MODEL_LABEL[review.model] },
                { label: 'Experiment', value: <Code>{review.experiment_id}</Code> },
                ...FIELDS.map((field) => ({ label: field.label, value: review[field.key] })),
              ]}
            />
            <div>
              <h3 className={styles.sectionTitle}>What the deployment does</h3>
              <ul className={styles.list}>
                {DEPLOYMENT_RULES.map((text) => (
                  <li key={text}>{text}</li>
                ))}
              </ul>
            </div>
          </div>
        )}
        <DeploymentCreationNotice error={create.error} created={review === null ? create.data : undefined} />
        <div className={styles.actions}>
          {review !== null ? (
            <>
              <Button type="submit" variant="primary" disabled={create.isPending}>
                {create.isPending ? 'Verifying the model…' : 'Create deployment'}
              </Button>
              <Button
                disabled={create.isPending}
                onClick={() => {
                  setReview(null);
                }}
              >
                Edit
              </Button>
            </>
          ) : (
            <Button type="submit" variant="primary" disabled={!fitted || blank !== undefined || badIdentifier}>
              Review deployment
            </Button>
          )}
        </div>
        {review === null && !fitted && (
          <p className={styles.note}>The {MODEL_LABEL[model].toLowerCase()} model is unevaluable and cannot be deployed.</p>
        )}
        {review === null && <p className={styles.note}>Simulated orders only; nothing is ever sent to a broker.</p>}
      </form>
    </Panel>
  );
}
