import { useId, useState } from 'react';

import type { ComparisonRequest } from '../../api/comparisonSchemas';
import type { ModelListing } from '../../api/modelSchemas';
import type { ExperimentPage, RunConfiguration, WindowName } from '../../api/researchSchemas';
import { CUTOFF_BASIS, FAMILY_LABEL, ORIGIN_LABEL, modelName } from '../../presentation/models';
import { WINDOW_LABEL, shortId } from '../../presentation/research';
import { useStartComparison } from '../../state/comparisons';
import { useModels } from '../../state/models';
import { ModelJobStatus } from '../models/ModelJobStatus';
import { CONFIGURATION_FIELDS, ConfigurationFields, FIXED_ASSUMPTIONS, fieldOf } from '../research/EvaluationForm';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';

const MIN_MODELS = 2;
const MAX_MODELS = 6;
const WINDOWS: readonly WindowName[] = ['validation', 'test'];

// What a comparison adds to every evaluation's assumptions.
const FAIRNESS = [
  'Every member is predicted on the same feature rows of the same split, instrument, and window, the unlabelled tail included, with the same configuration over the same recorded events.',
  'Each member trades in its own paper accounts beside its own cash and buy-and-hold benchmarks; no member shares liquidity, orders, or fills with another.',
  'No member is retrained. A member whose information cutoff is after the window starts, or unknown, is refused; one whose cutoff its uploader declared is compared, and marked conditional on that declaration.',
] as const;

/** Whether a version's known cutoff is after the window starts; RFC 3339 times of one form compare as text. */
function cutoffAfter(model: ModelListing, windowStart: string): boolean {
  const cutoff = model.training.information_cutoff;
  return cutoff.basis !== 'unavailable' && cutoff.at > windowStart;
}

// Compares two to six active model versions under one frozen spec. Nothing
// is sent until the reviewed spec is confirmed.
export function ComparisonForm({
  experiments,
  defaults,
}: {
  readonly experiments: ExperimentPage | undefined;
  readonly defaults: RunConfiguration;
}) {
  const formId = useId();
  const list = experiments?.experiments ?? [];
  const [experimentId, setExperimentId] = useState<string | null>(null);
  const experiment = list.find((entry) => entry.experiment_id === experimentId) ?? list[0];
  const [instrumentChoice, setInstrument] = useState<string | null>(null);
  const instrument =
    experiment?.instruments.find((entry) => entry.instrument === instrumentChoice)?.instrument ??
    experiment?.instruments[0]?.instrument;
  const [evaluationWindow, setWindow] = useState<WindowName>('test');
  const [chosen, setChosen] = useState<readonly string[]>([]);
  const [configuration, setConfiguration] = useState<RunConfiguration>(defaults);
  const [review, setReview] = useState<ComparisonRequest | null>(null);
  const models = useModels('active');
  const start = useStartComparison();

  if (experiment === undefined || instrument === undefined) {
    return (
      <Panel title="Compare model versions">
        <p className={styles.note}>There is no verified split to compare on yet. Build one with the research demo first.</p>
      </Panel>
    );
  }
  const windowStart = experiment.windows[evaluationWindow].start_at;
  const candidates = (models.data?.models ?? []).filter((model) => model.instrument === instrument);
  const selected = chosen.filter((id) => candidates.some((model) => model.model_version_id === id));
  const blank = CONFIGURATION_FIELDS.find((field) => configuration[field.key].trim() === '');
  const ready = selected.length >= MIN_MODELS && selected.length <= MAX_MODELS && blank === undefined;
  const nameOf = (id: string) => {
    const model = candidates.find((entry) => entry.model_version_id === id);
    return model === undefined ? shortId(id) : modelName(model);
  };

  if (review !== null) {
    return (
      <Panel title="Compare model versions" meta="Review">
        <div className={styles.review} aria-label="Review the comparison">
          <Facts
            facts={[
              { label: 'Split', value: <span className={styles.ids}>{review.split_artifact_id}</span> },
              { label: 'Stream', value: experiment.dataset.stream },
              { label: 'Instrument', value: review.instrument },
              { label: 'Window', value: WINDOW_LABEL[review.window] },
              { label: 'Window starts', value: <MarketTime value={experiment.windows[review.window].start_at} stacked /> },
              ...CONFIGURATION_FIELDS.map((field) => ({ label: field.label, value: review.configuration[field.key] })),
            ]}
          />
          <div className={styles.section}>
            <h3 className={styles.sectionTitle}>Members, in order</h3>
            <ol className={styles.list}>
              {review.model_version_ids.map((id) => (
                <li key={id}>
                  {nameOf(id)} <span className={styles.ids}>{id}</span>
                </li>
              ))}
            </ol>
            <p className={styles.note}>The first member that completes is the reference the others are compared with.</p>
          </div>
          <div className={styles.section}>
            <h3 className={styles.sectionTitle}>Assumptions</h3>
            <ul className={styles.list}>
              {[...FIXED_ASSUMPTIONS, ...FAIRNESS].map((text) => (
                <li key={text}>{text}</li>
              ))}
            </ul>
          </div>
          {start.data !== undefined ? (
            <ModelJobStatus jobId={start.data.job.job_id} kind="comparison" />
          ) : (
            start.error !== null && <ResearchFailure error={start.error} title="The comparison was not started" />
          )}
          {fieldOf(start.error) !== null && <p className={styles.fieldError}>Refused field: {fieldOf(start.error)}</p>}
          <div className={styles.actions}>
            {start.data === undefined && (
              <Button
                variant="primary"
                disabled={start.isPending}
                onClick={() => {
                  start.mutate(review);
                }}
              >
                {start.isPending ? 'Starting…' : 'Start comparison'}
              </Button>
            )}
            <Button
              onClick={() => {
                setReview(null);
                start.reset();
              }}
            >
              {start.data === undefined ? 'Edit' : 'Compare others'}
            </Button>
          </div>
        </div>
      </Panel>
    );
  }

  return (
    <Panel title="Compare model versions" meta={`${String(MIN_MODELS)} to ${String(MAX_MODELS)} active versions`}>
      <form
        className={styles.review}
        onSubmit={(event) => {
          event.preventDefault();
          if (!ready) return;
          // The reviewed spec stays fixed even if the library changes.
          setReview({
            model_version_ids: [...selected],
            split_artifact_id: experiment.split_artifact_id,
            instrument,
            window: evaluationWindow,
            configuration: { ...configuration },
          });
        }}
      >
        <div className={styles.fields}>
          <div className={styles.field}>
            <label htmlFor={`${formId}-split`}>Split</label>
            <select
              id={`${formId}-split`}
              value={experiment.experiment_id}
              onChange={(event) => {
                setExperimentId(event.target.value);
                setInstrument(null);
              }}
            >
              {list.map((entry) => (
                <option key={entry.experiment_id} value={entry.experiment_id}>
                  {shortId(entry.split_artifact_id)} · {entry.dataset.stream}
                </option>
              ))}
            </select>
          </div>
          <div className={styles.field}>
            <label htmlFor={`${formId}-instrument`}>Instrument</label>
            <select
              id={`${formId}-instrument`}
              value={instrument}
              onChange={(event) => {
                setInstrument(event.target.value);
              }}
            >
              {experiment.instruments.map((entry) => (
                <option key={entry.instrument} value={entry.instrument}>
                  {entry.instrument}
                </option>
              ))}
            </select>
          </div>
        </div>
        <fieldset className={styles.choices}>
          <legend>Window</legend>
          <div className={styles.inline}>
            {WINDOWS.map((name) => (
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
        <fieldset className={styles.choices}>
          <legend>Model versions for {instrument}</legend>
          {models.status === 'pending' ? (
            <p role="status">Loading model versions…</p>
          ) : candidates.length === 0 ? (
            <p className={styles.note}>No active model version predicts {instrument}. Register or import one first.</p>
          ) : (
            <div className={styles.review}>
              {candidates.map((model) => {
                const cutoff = model.training.information_cutoff;
                const basis = CUTOFF_BASIS[cutoff.basis];
                const checked = selected.includes(model.model_version_id);
                return (
                  <label key={model.model_version_id} className={styles.choice}>
                    <input
                      type="checkbox"
                      checked={checked}
                      disabled={!checked && selected.length >= MAX_MODELS}
                      onChange={() => {
                        setChosen(
                          checked
                            ? selected.filter((id) => id !== model.model_version_id)
                            : [...selected, model.model_version_id],
                        );
                      }}
                    />
                    {modelName(model)}
                    <span className={styles.choiceDetail}>
                      {FAMILY_LABEL[model.family]} · {ORIGIN_LABEL[model.origin]} ·{' '}
                      <Badge tone={basis.tone}>{basis.label}</Badge>
                      {cutoffAfter(model, windowStart) && ' Its cutoff is after this window starts: it will be refused.'}
                    </span>
                  </label>
                );
              })}
            </div>
          )}
        </fieldset>
        <div className={styles.section}>
          <h3 className={styles.sectionTitle}>Policy, capital, costs, and risk, the same for every member</h3>
          <ConfigurationFields
            idPrefix={formId}
            configuration={configuration}
            rejectedField={null}
            onChange={setConfiguration}
          />
        </div>
        <div className={styles.actions}>
          <Button type="submit" variant="primary" disabled={!ready}>
            Review comparison
          </Button>
        </div>
      </form>
    </Panel>
  );
}
