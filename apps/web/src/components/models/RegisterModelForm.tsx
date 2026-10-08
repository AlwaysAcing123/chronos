import { useId, useState } from 'react';

import { ChronosApiError } from '../../api/errors';
import type { ExperimentFamily, RegistrationRequest } from '../../api/modelSchemas';
import type { ExperimentPage } from '../../api/researchSchemas';
import { FAMILY_LABEL } from '../../presentation/models';
import { shortId } from '../../presentation/research';
import { useStartRegistration } from '../../state/models';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Button } from '../ui/Button';
import { Facts } from '../ui/Facts';
import { Panel } from '../ui/Panel';
import { ModelJobStatus } from './ModelJobStatus';

const NAME = /^[a-z0-9][a-z0-9._-]{0,63}$/;
const VERSION = /^(0|[1-9][0-9]{0,5})(\.(0|[1-9][0-9]{0,5})){0,2}$/;
const FAMILIES: readonly ExperimentFamily[] = ['logistic', 'majority'];

// Registers one fitted model of a verified experiment as an immutable
// model version. Nothing is sent until the reviewed request is confirmed.
export function RegisterModelForm({ experiments }: { readonly experiments: ExperimentPage | undefined }) {
  const formId = useId();
  const list = experiments?.experiments ?? [];
  const [experimentId, setExperimentId] = useState<string | null>(null);
  const experiment = list.find((entry) => entry.experiment_id === experimentId) ?? list[0];
  const [instrumentChoice, setInstrument] = useState<string | null>(null);
  const instrument =
    experiment?.instruments.find((entry) => entry.instrument === instrumentChoice) ?? experiment?.instruments[0];
  const [family, setFamily] = useState<ExperimentFamily>('logistic');
  const [name, setName] = useState('');
  const [version, setVersion] = useState('1');
  const [review, setReview] = useState<RegistrationRequest | null>(null);
  const start = useStartRegistration();

  if (experiment === undefined || instrument === undefined) {
    return (
      <Panel title="Register a model version">
        <p className={styles.note}>
          There is no verified experiment to register a model from yet. Build one with the research demo first.
        </p>
      </Panel>
    );
  }
  const fitted = instrument.models[family].status === 'fitted';
  const suggested = `${instrument.instrument.toLowerCase()}-${family}`;
  const chosenName = name === '' ? suggested : name;
  const nameValid = NAME.test(chosenName);
  const versionValid = VERSION.test(version);
  const rejected =
    start.error instanceof ChronosApiError && start.error.failure.kind === 'api_error'
      ? start.error.failure.error
      : null;

  if (review !== null) {
    return (
      <Panel title="Register a model version" meta="Review">
        <div className={styles.review}>
          <Facts
            facts={[
              {
                label: 'Experiment',
                value: <span className={styles.ids}>{review.experiment_id}</span>,
              },
              { label: 'Instrument', value: review.instrument },
              { label: 'Model', value: FAMILY_LABEL[review.model] },
              {
                label: 'Name',
                value: <span className="mono">{review.name}</span>,
              },
              {
                label: 'Version',
                value: <span className="mono">{review.version}</span>,
              },
            ]}
          />
          <p className={styles.note}>
            The research service verifies the experiment and reproduces its split, labels, and features from their
            sources before it publishes the version. The name and version then name these exact bytes for good; they
            cannot later point to another model.
          </p>
          {start.data !== undefined ? (
            <ModelJobStatus jobId={start.data.job.job_id} kind="registration" />
          ) : (
            start.error !== null && <ResearchFailure error={start.error} title="The registration was not accepted" />
          )}
          <div className={styles.actions}>
            {start.data === undefined && (
              <Button
                variant="primary"
                disabled={start.isPending}
                onClick={() => {
                  start.mutate(review);
                }}
              >
                {start.isPending ? 'Registering…' : 'Register'}
              </Button>
            )}
            <Button
              onClick={() => {
                setReview(null);
                start.reset();
              }}
            >
              {start.data === undefined ? 'Edit' : 'Register another'}
            </Button>
          </div>
          {rejected?.field !== null && rejected?.field !== undefined && (
            <p className={styles.fieldError}>Refused field: {rejected.field}</p>
          )}
        </div>
      </Panel>
    );
  }

  return (
    <Panel title="Register a model version" meta="From a verified experiment">
      <form
        className={styles.review}
        aria-describedby={`${formId}-note`}
        onSubmit={(event) => {
          event.preventDefault();
          setReview({
            experiment_id: experiment.experiment_id,
            instrument: instrument.instrument,
            model: family,
            name: chosenName,
            version,
          });
        }}
      >
        <div className={styles.fields}>
          <div className={styles.field}>
            <label htmlFor={`${formId}-experiment`}>Experiment</label>
            <select
              id={`${formId}-experiment`}
              value={experiment.experiment_id}
              onChange={(event) => {
                setExperimentId(event.target.value);
                setInstrument(null);
              }}
            >
              {list.map((entry) => (
                <option key={entry.experiment_id} value={entry.experiment_id}>
                  {shortId(entry.experiment_id)} · {entry.dataset.stream}
                </option>
              ))}
            </select>
          </div>
          <div className={styles.field}>
            <label htmlFor={`${formId}-instrument`}>Instrument</label>
            <select
              id={`${formId}-instrument`}
              value={instrument.instrument}
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
          <div className={styles.field}>
            <label htmlFor={`${formId}-model`}>Model</label>
            <select
              id={`${formId}-model`}
              value={family}
              onChange={(event) => {
                setFamily(event.target.value as ExperimentFamily);
              }}
            >
              {FAMILIES.map((option) => (
                <option key={option} value={option} disabled={instrument.models[option].status !== 'fitted'}>
                  {FAMILY_LABEL[option]}
                  {instrument.models[option].status === 'fitted' ? '' : ' (unevaluable)'}
                </option>
              ))}
            </select>
          </div>
          <div className={styles.field}>
            <label htmlFor={`${formId}-name`}>Name</label>
            <input
              id={`${formId}-name`}
              value={name}
              placeholder={suggested}
              autoComplete="off"
              spellCheck={false}
              aria-invalid={!nameValid}
              aria-describedby={`${formId}-name-hint`}
              onChange={(event) => {
                setName(event.target.value);
              }}
            />
            <span id={`${formId}-name-hint`} className={nameValid ? styles.fieldHint : styles.fieldError}>
              Lowercase letters, digits, dots, dashes, or underscores
            </span>
          </div>
          <div className={styles.field}>
            <label htmlFor={`${formId}-version`}>Version</label>
            <input
              id={`${formId}-version`}
              value={version}
              autoComplete="off"
              spellCheck={false}
              aria-invalid={!versionValid}
              aria-describedby={`${formId}-version-hint`}
              onChange={(event) => {
                setVersion(event.target.value);
              }}
            />
            <span id={`${formId}-version-hint`} className={versionValid ? styles.fieldHint : styles.fieldError}>
              Such as 1 or 1.2.0
            </span>
          </div>
        </div>
        <p id={`${formId}-note`} className={styles.note}>
          Only a fitted model can be registered. Its parameters, inputs, target, and verified training cutoff become one
          immutable version.
        </p>
        <div className={styles.actions}>
          <Button type="submit" variant="primary" disabled={!fitted || !nameValid || !versionValid}>
            Review registration
          </Button>
        </div>
      </form>
    </Panel>
  );
}
