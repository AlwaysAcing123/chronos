import { useId, useState } from 'react';

import { ChronosApiError } from '../../api/errors';
import type { ExperimentPage, ModelName, RunConfiguration, RunRequest, WindowName } from '../../api/researchSchemas';
import type { StartedRun } from '../../api/research';
import type { UnsignedInteger } from '../../api/schemas';
import { nanosecondsAsSeconds } from '../../presentation/format';
import { JOB_STATE, MODEL_LABEL, WINDOW_LABEL, ppbAsPercent } from '../../presentation/research';
import { researchHref } from '../../state/route';
import { useStartRun } from '../../state/research';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import type { Selection } from './ModelCatalog';
import styles from './research.module.css';

// What every evaluation assumes, whatever its configuration.
export const FIXED_ASSUMPTIONS = [
  'One instrument per run, long or flat and never short: an up prediction at or above the threshold targets the target position, a down one targets flat, anything else holds.',
  'Market immediate-or-cancel orders, filled at the displayed opposite best price and never beyond its displayed size; there is no depth, queue position, or market impact.',
  'An order executes on the first quote of its instrument received at or after its arrival, so a signal never fills on its own quote.',
  'Fresh paper accounts for the strategy and for the cash and buy-and-hold benchmarks; no trading account, replay, or live source is touched.',
  'Equity marks a position at the best bid; a position that cannot be marked is reported as unavailable, never valued at zero.',
] as const;

interface FieldSpec {
  readonly key: keyof RunConfiguration;
  readonly label: string;
  readonly hint: (value: string) => string | null;
}

const INTEGER = /^(0|[1-9][0-9]*)$/;

const FIELDS: readonly FieldSpec[] = [
  {
    key: 'threshold_ppb',
    label: 'Confidence threshold (parts per billion)',
    hint: (value) => (INTEGER.test(value) ? `The predicted class needs at least ${ppbAsPercent(value)}%` : null),
  },
  { key: 'target_quantity', label: 'Target position (shares)', hint: () => null },
  {
    key: 'latency_ns',
    label: 'Signal-to-order latency (ns)',
    hint: (value) => (INTEGER.test(value) ? `${nanosecondsAsSeconds(value as UnsignedInteger)} s` : null),
  },
  {
    key: 'decision_interval_ns',
    label: 'Minimum time between decisions (ns)',
    hint: (value) => (INTEGER.test(value) ? `${nanosecondsAsSeconds(value as UnsignedInteger)} s` : null),
  },
  { key: 'starting_cash', label: 'Starting cash (USD)', hint: () => null },
  { key: 'fee_per_fill', label: 'Fee per fill (USD)', hint: () => 'An assumption, not a broker’s fee schedule' },
  {
    key: 'maximum_quote_age_ns',
    label: 'Maximum quote age for marks (ns)',
    hint: (value) => (INTEGER.test(value) ? `${nanosecondsAsSeconds(value as UnsignedInteger)} s` : null),
  },
  { key: 'maximum_order_notional', label: 'Maximum order notional (USD)', hint: () => null },
];

function fieldOf(error: unknown): string | null {
  return error instanceof ChronosApiError && error.failure.kind === 'api_error' ? error.failure.error.field : null;
}

function StartedNotice({ started }: { readonly started: StartedRun }) {
  const state = JOB_STATE[started.job.state];
  return (
    <Alert
      tone={started.deduplicated ? 'info' : 'positive'}
      title={started.deduplicated ? 'This evaluation already exists' : 'Evaluation queued'}
      live
    >
      <p>
        {started.deduplicated
          ? 'An identical request was already queued, running, or finished, so no new work was started.'
          : 'It runs in the research service; its status appears under Runs.'}{' '}
        <Badge tone={state.tone}>{state.label}</Badge>
      </p>
      {started.job.result !== null && (
        <p>
          <a href={researchHref(started.job.result.run_id)}>Open the result</a>
        </p>
      )}
    </Alert>
  );
}

export function EvaluationForm({
  selection,
  defaults,
  experiments,
}: {
  readonly selection: Selection | null;
  readonly defaults: RunConfiguration;
  readonly experiments: ExperimentPage | undefined;
}) {
  const formId = useId();
  const [model, setModel] = useState<ModelName>('logistic');
  const [evaluationWindow, setWindow] = useState<WindowName>('validation');
  const [configuration, setConfiguration] = useState<RunConfiguration>(defaults);
  const [review, setReview] = useState<RunRequest | null>(null);
  const reviewing = review !== null;
  const start = useStartRun();

  const experiment = experiments?.experiments.find((entry) => entry.experiment_id === selection?.experimentId);
  const entry = experiment?.instruments.find((item) => item.instrument === selection?.instrument);
  if (selection === null || experiment === undefined || entry === undefined) {
    return (
      <Panel title="Evaluation">
        <p className={styles.note}>Choose a model with a fitted instrument to configure an evaluation.</p>
      </Panel>
    );
  }
  const chosen = entry.models[model];
  const fittedModel = chosen.status === 'fitted';
  const rejectedField = fieldOf(start.error);
  const blank = FIELDS.find((field) => configuration[field.key].trim() === '');

  const request: RunRequest = {
    experiment_id: experiment.experiment_id,
    instrument: entry.instrument,
    model,
    window: evaluationWindow,
    configuration: { ...configuration },
  };

  return (
    <Panel title="Evaluation" meta={`${review?.instrument ?? entry.instrument} · ${MODEL_LABEL[review?.model ?? model]}`}>
      <form
        id={formId}
        onSubmit={(event) => {
          event.preventDefault();
          if (start.isPending) return;
          if (review === null) {
            if (!fittedModel || blank !== undefined) return;
            // The reviewed intent stays fixed even if the catalog changes.
            setReview(request);
            return;
          }
          start.mutate(review, {
            onSuccess: () => {
              setReview(null);
            },
          });
        }}
      >
        {!reviewing ? (
          <>
            <div className={`${styles.inline ?? ''} ${styles.section ?? ''}`}>
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
              <fieldset className={styles.choices}>
                <legend>Window</legend>
                <div className={styles.inline}>
                  {(['validation', 'test'] as const).map((name) => (
                    <label key={name} className={styles.choice}>
                      <input
                        type="radio"
                        name={`${formId}-window`}
                        value={name}
                        checked={evaluationWindow === name}
                        onChange={() => {
                          setWindow(name);
                        }}
                      />
                      {WINDOW_LABEL[name]}
                      <span className={styles.choiceDetail}>
                        from <MarketTime value={experiment.windows[name].start_at} />
                      </span>
                    </label>
                  ))}
                </div>
              </fieldset>
            </div>
            {!fittedModel && (
              <Alert tone="caution" title={`The ${MODEL_LABEL[model].toLowerCase()} model is unevaluable`}>
                <p>
                  It predicts nothing for {entry.instrument}, so it cannot be evaluated.{' '}
                  <Code>{chosen.reason ?? 'unevaluable'}</Code>
                </p>
              </Alert>
            )}
            <div className={styles.section}>
              <h3 className={styles.sectionTitle}>Policy, capital, costs, and risk</h3>
              <div className={styles.fields}>
                {FIELDS.map((field) => {
                  const id = `${formId}-${field.key}`;
                  const hint = field.hint(configuration[field.key]);
                  const invalid = rejectedField === `configuration.${field.key}`;
                  return (
                    <div key={field.key} className={styles.field}>
                      <label htmlFor={id}>{field.label}</label>
                      <input
                        id={id}
                        inputMode="decimal"
                        autoComplete="off"
                        spellCheck={false}
                        value={configuration[field.key]}
                        aria-invalid={invalid}
                        aria-describedby={hint === null ? undefined : `${id}-hint`}
                        onChange={(event) => {
                          setConfiguration({ ...configuration, [field.key]: event.target.value });
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
              <p className={styles.note}>
                Defaults were fixed before any evaluation was run and were not tuned on test results.
              </p>
            </div>
          </>
        ) : (
          <div className={styles.review} aria-label="Review the evaluation">
            <Facts
              facts={[
                { label: 'Instrument', value: review.instrument },
                { label: 'Model', value: MODEL_LABEL[review.model] },
                { label: 'Window', value: WINDOW_LABEL[review.window] },
                ...FIELDS.map((field) => ({ label: field.label, value: review.configuration[field.key] })),
              ]}
            />
            <div>
              <h3 className={styles.sectionTitle}>Assumptions</h3>
              <ul className={styles.list}>
                {FIXED_ASSUMPTIONS.map((text) => (
                  <li key={text}>{text}</li>
                ))}
              </ul>
            </div>
          </div>
        )}
        {start.error !== null && (
          <div className={styles.section}>
            <Alert tone="negative" title="The evaluation was not started" live>
              {start.error instanceof ChronosApiError && start.error.failure.kind === 'api_error' ? (
                <p>
                  {start.error.failure.error.message}{' '}
                  <Code>{start.error.failure.error.reason ?? start.error.failure.error.code}</Code>
                </p>
              ) : (
                <p>{start.error.message}</p>
              )}
            </Alert>
          </div>
        )}
        {start.data !== undefined && !reviewing && (
          <div className={styles.section}>
            <StartedNotice started={start.data} />
          </div>
        )}
        <div className={styles.actions}>
          {reviewing ? (
            <>
              <Button type="submit" variant="primary" disabled={start.isPending}>
                {start.isPending ? 'Starting…' : 'Start evaluation'}
              </Button>
              <Button
                disabled={start.isPending}
                onClick={() => {
                  setReview(null);
                }}
              >
                Edit
              </Button>
            </>
          ) : (
            <Button type="submit" variant="primary" disabled={!fittedModel || blank !== undefined}>
              Review evaluation
            </Button>
          )}
        </div>
      </form>
    </Panel>
  );
}
