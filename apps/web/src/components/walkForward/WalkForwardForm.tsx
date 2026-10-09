import { useId, useState } from 'react';

import type { RunConfiguration } from '../../api/researchSchemas';
import type { Collection, ScenarioDraft, WalkForwardRequest } from '../../api/walkForwardSchemas';
import { JOB_STATE, shortId } from '../../presentation/research';
import { EVIDENCE, scenarioLabel, studyReason } from '../../presentation/walkForward';
import { walkForwardHref } from '../../state/route';
import { useCollections, usePreviewWalkForward, useWalkForwardJob } from '../../state/walkForward';
import { CONFIGURATION_FIELDS, ConfigurationFields, fieldOf } from '../research/EvaluationForm';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { cx } from '../ui/cx';
import { Facts } from '../ui/Facts';
import { Panel } from '../ui/Panel';
import { CREATE_COLLECTION_COMMAND } from './CollectionList';

const STRESS_DEFAULT = { fee_per_fill: '0.05', latency_ns: '5000000' } as const;

function recordings(count: number): string {
  return `${String(count)} recording${count === 1 ? '' : 's'}`;
}

/** How many of a collection's members hold the instrument. */
function holding(collection: Collection, instrument: string): number {
  return Number(collection.instruments[instrument] ?? '0');
}

// Why a study cannot be planned with these recordings, and what is needed.
export function InsufficientCoverage({
  instrument,
  held,
  minimum,
}: {
  readonly instrument: string;
  readonly held: number;
  readonly minimum: number;
}) {
  return (
    <Alert tone="caution" title="Not enough independent recordings for a walk-forward study">
      <p>
        This collection holds {held} recording{held === 1 ? '' : 's'} of {instrument}. A study fits each fold on at
        least {minimum} earlier recording{minimum === 1 ? '' : 's'} and measures it on a later one, so it needs at least{' '}
        {minimum + 1} independent recordings of {instrument}, each its own verified dataset and stream.
      </p>
      <p>
        Record or obtain more sessions, build each one’s dataset, features, and labels, and collect them with{' '}
        <Code>{CREATE_COLLECTION_COMMAND}</Code>. Chronos never splits, clones, or shifts a recording to make more.
      </p>
    </Alert>
  );
}

function PlanJobStatus({ jobId }: { readonly jobId: string }) {
  const job = useWalkForwardJob(jobId);
  if (job.data === undefined) {
    return job.status === 'error' ? (
      <ResearchFailure error={job.error} title="The preview’s status could not be read" />
    ) : (
      <p role="status">Reading the preview…</p>
    );
  }
  const state = JOB_STATE[job.data.state];
  const { error } = job.data;
  const result = job.data.kind === 'walk_forward_plan' ? job.data.result : null;
  return (
    <Alert tone={state.tone} title={job.data.state === 'succeeded' ? 'The plan is ready to review' : `Preview ${state.label.toLowerCase()}`} live>
      <p>
        <Badge tone={state.tone}>{state.label}</Badge>{' '}
        {job.data.state === 'queued' || job.data.state === 'running'
          ? 'Verifying every recording with the C++ tools and fixing the folds, cutoffs, and workload…'
          : null}
      </p>
      {result !== null && (
        <p>
          {result.outcome === 'already_present' ? 'This exact plan was already published. ' : ''}
          {result.folds} folds. <a href={walkForwardHref({ planId: result.plan_id })}>Review the plan</a>
        </p>
      )}
      {error !== null && (
        <p>
          {error.reason === undefined ? error.message : `${studyReason(error.reason)}: ${error.message}`}
          <span className={styles.reasonCode}>{error.reason ?? error.code}</span>
        </p>
      )}
    </Alert>
  );
}

// Plans a study over one verified collection and instrument. Preview sends
// the request exactly as shown and publishes its frozen plan; nothing runs
// until the plan is reviewed and started.
export function WalkForwardForm({ defaults }: { readonly defaults: RunConfiguration }) {
  const formId = useId();
  const collections = useCollections();
  const list = collections.data?.collections ?? [];
  const [collectionId, setCollectionId] = useState<string | null>(null);
  const collection = list.find((entry) => entry.collection_id === collectionId) ?? list[0];
  const [instrumentChoice, setInstrument] = useState<string | null>(null);
  const instruments = collection === undefined ? [] : Object.keys(collection.instruments);
  const instrument = instruments.find((name) => name === instrumentChoice) ?? instruments[0];
  const [minimumChoice, setMinimum] = useState('1');
  const [configuration, setConfiguration] = useState<RunConfiguration>(defaults);
  const [stress, setStress] = useState(true);
  const [stressCell, setStressCell] = useState<{ fee_per_fill: string; latency_ns: string }>({ ...STRESS_DEFAULT });
  const [zeroFee, setZeroFee] = useState(false);
  const [submitted, setSubmitted] = useState<WalkForwardRequest | null>(null);
  const preview = usePreviewWalkForward();

  if (collections.status === 'pending') {
    return (
      <Panel title="Plan a walk-forward study">
        <p role="status">Loading collections…</p>
      </Panel>
    );
  }
  if (collection === undefined || instrument === undefined) {
    return (
      <Panel title="Plan a walk-forward study">
        <p className={styles.note}>
          There is no verified collection of recordings to plan on yet. Collect two or more independent recordings with{' '}
          <Code>{CREATE_COLLECTION_COMMAND}</Code>
        </p>
      </Panel>
    );
  }
  const held = holding(collection, instrument);
  const options = Array.from({ length: Math.max(1, held - 1) }, (_, index) => String(index + 1));
  const minimum = options.includes(minimumChoice) ? minimumChoice : '1';
  const enough = held >= Number(minimum) + 1;
  const blank =
    CONFIGURATION_FIELDS.some((field) => configuration[field.key].trim() === '') ||
    (stress && (stressCell.fee_per_fill.trim() === '' || stressCell.latency_ns.trim() === ''));
  const scenarios: ScenarioDraft[] = [
    { name: 'base', fee_per_fill: configuration.fee_per_fill, latency_ns: configuration.latency_ns },
    ...(stress ? [{ name: 'stress', ...stressCell }] : []),
    ...(stress && zeroFee ? [{ name: 'zero_fee', fee_per_fill: '0', latency_ns: configuration.latency_ns }] : []),
  ];

  if (submitted !== null) {
    return (
      <Panel title="Plan a walk-forward study" meta="Preview">
        <div className={styles.review} aria-label="The previewed request">
          <Facts
            facts={[
              { label: 'Collection', value: <span className={styles.ids}>{submitted.collection_id}</span> },
              { label: 'Instrument', value: submitted.instrument },
              { label: 'Earliest fold trains on', value: `${submitted.minimum_training_sources} recording(s)` },
              ...submitted.scenarios.map((cell) => ({
                label: `${scenarioLabel(cell.name)} scenario`,
                value: `${cell.fee_per_fill} USD per fill, ${cell.latency_ns} ns latency`,
              })),
            ]}
          />
          {preview.data !== undefined ? (
            <PlanJobStatus jobId={preview.data.job.job_id} />
          ) : (
            preview.error !== null && <ResearchFailure error={preview.error} title="The plan was not previewed" />
          )}
          {fieldOf(preview.error) !== null && <p className={styles.fieldError}>Refused field: {fieldOf(preview.error)}</p>}
          <div className={styles.actions}>
            <Button
              onClick={() => {
                setSubmitted(null);
                preview.reset();
              }}
            >
              Edit
            </Button>
          </div>
        </div>
      </Panel>
    );
  }

  return (
    <Panel title="Plan a walk-forward study" meta="One instrument, expanding folds">
      <form
        className={styles.review}
        onSubmit={(event) => {
          event.preventDefault();
          if (!enough || blank) return;
          // Exactly this request is previewed, whatever changes afterwards.
          const request: WalkForwardRequest = {
            collection_id: collection.collection_id,
            instrument,
            minimum_training_sources: minimum,
            configuration: { ...configuration },
            scenarios: scenarios.map((cell) => ({ ...cell })),
          };
          setSubmitted(request);
          preview.mutate(request);
        }}
      >
        <div className={styles.fields}>
          <div className={styles.field}>
            <label htmlFor={`${formId}-collection`}>Collection</label>
            <select
              id={`${formId}-collection`}
              value={collection.collection_id}
              onChange={(event) => {
                setCollectionId(event.target.value);
                setInstrument(null);
              }}
            >
              {list.map((entry) => (
                <option key={entry.collection_id} value={entry.collection_id}>
                  {shortId(entry.collection_id)} · {recordings(entry.members.length)} · {EVIDENCE[entry.evidence].label}
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
              {instruments.map((name) => (
                <option key={name} value={name}>
                  {name} · in {recordings(Number(collection.instruments[name] ?? '0'))}
                </option>
              ))}
            </select>
          </div>
          <div className={styles.field}>
            <label htmlFor={`${formId}-minimum`}>Earliest fold trains on</label>
            <select
              id={`${formId}-minimum`}
              value={minimum}
              onChange={(event) => {
                setMinimum(event.target.value);
              }}
            >
              {options.map((value) => (
                <option key={value} value={value}>
                  {value} recording{value === '1' ? '' : 's'}
                </option>
              ))}
            </select>
            <span className={styles.fieldHint}>Each later fold also trains on every earlier recording</span>
          </div>
        </div>
        {collection.evidence === 'synthetic' && (
          <p className={styles.note}>
            <Badge tone={EVIDENCE.synthetic.tone}>{EVIDENCE.synthetic.label}</Badge> This collection is generated test
            data: a study of it tests the machinery, not a market.
          </p>
        )}
        {!enough && <InsufficientCoverage instrument={instrument} held={held} minimum={Number(minimum)} />}
        <div className={styles.section}>
          <h3 className={styles.sectionTitle}>Policy, capital, and base costs, the same for every fold</h3>
          <ConfigurationFields
            idPrefix={formId}
            configuration={configuration}
            rejectedField={fieldOf(preview.error)}
            onChange={setConfiguration}
          />
        </div>
        <fieldset className={cx(styles.choices, styles.section)}>
          <legend>Cost and latency scenarios</legend>
          <p className={styles.note}>
            The base scenario is the fee and latency above. Every scenario runs the same predictions through the engine
            in its own accounts; spreads and recorded prices never change.
          </p>
          <label className={styles.choice}>
            <input
              type="checkbox"
              checked={stress}
              onChange={() => {
                setStress(!stress);
              }}
            />
            Stress scenario
            <span className={styles.choiceDetail}>No cheaper and no faster than the base, and costlier or slower</span>
          </label>
          {stress && (
            <div className={styles.fields}>
              <div className={styles.field}>
                <label htmlFor={`${formId}-stress-fee`}>Stress fee per fill (USD)</label>
                <input
                  id={`${formId}-stress-fee`}
                  inputMode="decimal"
                  autoComplete="off"
                  spellCheck={false}
                  value={stressCell.fee_per_fill}
                  onChange={(event) => {
                    setStressCell({ ...stressCell, fee_per_fill: event.target.value });
                  }}
                />
              </div>
              <div className={styles.field}>
                <label htmlFor={`${formId}-stress-latency`}>Stress latency (ns)</label>
                <input
                  id={`${formId}-stress-latency`}
                  inputMode="numeric"
                  autoComplete="off"
                  spellCheck={false}
                  value={stressCell.latency_ns}
                  onChange={(event) => {
                    setStressCell({ ...stressCell, latency_ns: event.target.value });
                  }}
                />
              </div>
            </div>
          )}
          <label className={styles.choice} data-disabled={!stress}>
            <input
              type="checkbox"
              checked={stress && zeroFee}
              disabled={!stress}
              onChange={() => {
                setZeroFee(!zeroFee);
              }}
            />
            Zero-fee reference
            <span className={styles.choiceDetail}>
              Explicitly selected only: the base latency with no fee. Needs the stress scenario.
            </span>
          </label>
        </fieldset>
        <div className={styles.actions}>
          <Button type="submit" variant="primary" disabled={!enough || blank}>
            Preview plan
          </Button>
        </div>
        {enough && (
          <p className={styles.note}>
            Preview verifies every recording and publishes the plan of exactly this request; nothing runs until you
            review it and start the study.
          </p>
        )}
      </form>
    </Panel>
  );
}
