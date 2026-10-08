import { useId, useState } from 'react';

import type { Compatibility, ModelDetail } from '../../api/modelSchemas';
import {
  FAMILY_LABEL,
  LIFECYCLE_LABEL,
  NOT_VERIFIED_LABEL,
  ORIGIN_LABEL,
  SOURCE_CHECK,
  VERIFICATION_LABEL,
  modelName,
  orderedStatements,
  sentence,
} from '../../presentation/models';
import { useModel, useSetModelLifecycle } from '../../state/models';
import historyStyles from '../history/history.module.css';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { Unavailable } from '../ui/DecimalText';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { DeployVersionForm } from './DeployVersionForm';
import { CutoffText } from './ModelLibrary';

function Ids({ children }: { readonly children: string }) {
  return <span className={styles.ids}>{children}</span>;
}

// Archiving changes only what new comparisons and deployments may choose;
// it never touches the version's bytes or anything already using it.
function LifecycleControl({ model }: { readonly model: ModelDetail }) {
  const formId = useId();
  const change = useSetModelLifecycle(model.model_version_id);
  const [reason, setReason] = useState('');
  const target = model.lifecycle.state === 'active' ? 'archived' : 'active';
  return (
    <form
      className={styles.section}
      onSubmit={(event) => {
        event.preventDefault();
        change.mutate({ state: target, reason: reason.trim() });
      }}
    >
      <h3 className={styles.sectionTitle}>{target === 'archived' ? 'Archive' : 'Restore'}</h3>
      <p className={styles.note}>
        {target === 'archived'
          ? 'An archived version cannot be chosen for a new comparison or deployment. Nothing already using it changes, and its bytes stay as they are.'
          : 'A restored version can be chosen again for new comparisons and deployments.'}
      </p>
      <div className={styles.fields}>
        <div className={styles.field}>
          <label htmlFor={`${formId}-reason`}>Reason, for the record</label>
          <input
            id={`${formId}-reason`}
            value={reason}
            maxLength={200}
            autoComplete="off"
            onChange={(event) => {
              setReason(event.target.value);
            }}
          />
        </div>
      </div>
      {change.error !== null && <ResearchFailure error={change.error} title="The lifecycle was not changed" />}
      <div className={styles.actions}>
        <Button type="submit" disabled={reason.trim() === '' || change.isPending}>
          {change.isPending ? 'Saving…' : target === 'archived' ? 'Archive this version' : 'Restore this version'}
        </Button>
      </div>
    </form>
  );
}

function CompatibilityRow({ label, entry }: { readonly label: string; readonly entry: Compatibility }) {
  return (
    <tr>
      <th scope="row">{label}</th>
      <td data-label="Status">
        <Badge tone={entry.status === 'compatible' ? 'positive' : 'caution'}>
          {entry.status === 'compatible' ? 'Compatible' : 'Unavailable'}
        </Badge>
      </td>
      <td className={styles.decisionOutcome} data-label="Requirement or reason">
        {sentence(entry.status === 'compatible' ? entry.requires : entry.detail)}
        {entry.status === 'unavailable' && <span className={styles.reasonCode}>{entry.reason}</span>}
      </td>
    </tr>
  );
}

function Identity({ model }: { readonly model: ModelDetail }) {
  const lifecycle = LIFECYCLE_LABEL[model.lifecycle.state];
  return (
    <Panel title={modelName(model)} meta={<Badge tone={lifecycle.tone}>{lifecycle.label}</Badge>}>
      <Facts
        facts={[
          {
            label: 'Model version ID',
            value: <Ids>{model.model_version_id}</Ids>,
          },
          {
            label: 'Names',
            value:
              model.names.length === 0 ? (
                <Unavailable>None</Unavailable>
              ) : (
                model.names.map((entry) => `${entry.name} ${entry.version}`).join(', ')
              ),
          },
          { label: 'Family', value: FAMILY_LABEL[model.family] },
          {
            label: 'Instrument',
            value: <span className="numeric">{model.instrument}</span>,
          },
          {
            label: 'Format',
            value: `${model.format.name} ${model.format.version}`,
          },
          {
            label: 'Runtime',
            value: `${model.runtime.name} ${model.runtime.version}`,
          },
          {
            label: 'Payload SHA-256',
            value: <Ids>{model.payload.sha256}</Ids>,
          },
          {
            label: 'Payload size (bytes)',
            value: <span className="numeric">{model.payload.size_bytes}</span>,
          },
        ]}
      />
      <p className={styles.note}>
        Artifact integrity: every read checks the version&rsquo;s ID against its manifest and the payload against its
        size and checksum, through no symlink,{' '}
        {model.family === 'onnx'
          ? 'and never parses the model itself: only the ONNX sandbox does, checking it against its profile at every load.'
          : `and parses the payload as a valid ${FAMILY_LABEL[model.family]} model.`}
      </p>
      {model.lifecycle.state === 'archived' && model.lifecycle.changed_at !== null && (
        <p className={styles.note}>
          Archived at <MarketTime value={model.lifecycle.changed_at} />: {model.lifecycle.reason}
        </p>
      )}
      <LifecycleControl model={model} />
    </Panel>
  );
}

function Contract({ model }: { readonly model: ModelDetail }) {
  return (
    <Panel title="Inputs and outputs" meta={`${model.inputs.name} version ${model.inputs.version}`}>
      <div className={historyStyles.scroll}>
        <table className={historyStyles.table}>
          <caption className="visually-hidden">The model&rsquo;s inputs, in order</caption>
          <thead>
            <tr>
              <th scope="col">Input</th>
              <th scope="col">Definition</th>
              <th scope="col">Unit</th>
              <th scope="col" className={historyStyles.number}>
                Scale
              </th>
            </tr>
          </thead>
          <tbody>
            {model.inputs.fields.map((field) => (
              <tr key={field.name}>
                <td data-label="Input" className="mono">
                  {field.name}
                </td>
                <td data-label="Definition" className={styles.decisionOutcome}>
                  {field.definition}
                </td>
                <td data-label="Unit">{field.unit}</td>
                <td data-label="Scale" className={historyStyles.number}>
                  {field.scale}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <ul className={styles.list}>
        <li>{sentence(model.inputs.row)}</li>
        <li>Conversion: {sentence(model.inputs.conversion)}</li>
        <li>Inference: {sentence(model.runtime.description)}</li>
      </ul>
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>What it predicts</h3>
        <Facts
          facts={[
            { label: 'Target', value: model.target.label_set },
            {
              label: 'Horizon (ns)',
              value: <span className="numeric">{model.target.horizon_ns}</span>,
            },
            {
              label: 'Classes, in order',
              value: model.output.class_order.join(', '),
            },
          ]}
        />
        <ul className={styles.list}>
          <li>Target: {sentence(model.target.definition)}</li>
          <li>Output: {sentence(model.output.probabilities)}</li>
          <li>Predicted class: {sentence(model.runtime.class)}</li>
          <li>Confidence: {sentence(model.output.confidence_ppb)}</li>
        </ul>
      </div>
    </Panel>
  );
}

type Origin<Name extends ModelDetail['provenance']['origin']> = Extract<ModelDetail['provenance'], { origin: Name }>;

function Verified({ statements }: { readonly statements: Readonly<Record<string, string>> }) {
  return (
    <div className={styles.section}>
      <h3 className={styles.sectionTitle}>What Chronos verified</h3>
      <ul className={styles.list}>
        {orderedStatements(statements).map(([key, statement]) => (
          <li key={key}>
            <strong>{VERIFICATION_LABEL[key] ?? key.replaceAll('_', ' ')}.</strong> {sentence(statement)}
          </li>
        ))}
      </ul>
    </div>
  );
}

function NotVerified({ statements }: { readonly statements: readonly (readonly [string, string])[] }) {
  return (
    <div className={styles.section}>
      <h3 className={styles.sectionTitle}>Not verified</h3>
      <ul className={styles.list}>
        {statements.map(([label, statement]) => (
          <li key={label}>
            <strong>{label}.</strong> {sentence(statement)}
          </li>
        ))}
      </ul>
    </div>
  );
}

function CutoffMeaning({ model }: { readonly model: ModelDetail }) {
  const cutoff = model.training.information_cutoff;
  return cutoff.basis === 'unavailable' ? null : (
    <p className={styles.note}>The cutoff means: {sentence(cutoff.meaning)}</p>
  );
}

function ExperimentProvenance({
  model,
  provenance,
}: {
  readonly model: ModelDetail;
  readonly provenance: Origin<'experiment'>;
}) {
  const { training, source_check: sourceCheck } = model;
  const source = SOURCE_CHECK[sourceCheck.state];
  return (
    <Panel title="Training provenance" meta={ORIGIN_LABEL[provenance.origin]}>
      <Facts
        facts={[
          {
            label: 'Information cutoff (UTC)',
            value: <CutoffText cutoff={training.information_cutoff} />,
          },
          ...(training.labels_available_through === undefined
            ? []
            : [
                {
                  label: 'Training labels known by',
                  value: <MarketTime value={training.labels_available_through} stacked />,
                },
              ]),
          ...(training.rows === undefined
            ? []
            : [
                {
                  label: 'Training rows',
                  value: <span className="numeric">{training.rows}</span>,
                },
              ]),
        ]}
      />
      <CutoffMeaning model={model} />
      {training.class_counts !== undefined && (
        <p className={styles.note}>
          Training classes:{' '}
          {Object.entries(training.class_counts)
            .map(([name, count]) => `${name} ${count}`)
            .join(', ')}
        </p>
      )}
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Source</h3>
        <Facts
          facts={[
            {
              label: 'Experiment',
              value: <Ids>{provenance.experiment_id}</Ids>,
            },
            {
              label: 'Dataset',
              value: <Ids>{provenance.sources.dataset_id}</Ids>,
            },
            {
              label: 'Split artifact',
              value: <Ids>{provenance.sources.split_artifact_id}</Ids>,
            },
            { label: 'Stream', value: provenance.sources.stream },
            {
              label: 'Validation from',
              value: <MarketTime value={provenance.sources.validation_start_at} stacked />,
            },
          ]}
        />
        <p className={styles.note}>
          <Badge tone={source.tone}>{source.label}</Badge>{' '}
          {'detail' in sourceCheck && sourceCheck.detail !== undefined ? sentence(sourceCheck.detail) : null}
        </p>
        <p className={styles.note}>
          The version does not need its experiment: this only traces where it came from. Software:{' '}
          {Object.entries(provenance.environment)
            .filter(([name]) => ['python', 'numpy', 'scikit_learn'].includes(name))
            .map(([name, value]) => `${name.replace('_', '-')} ${value}`)
            .join(', ')}
          .
        </p>
      </div>
      <Verified statements={model.verification} />
      <NotVerified statements={Object.values(provenance.code).map((text) => ['Source code revision', text] as const)} />
    </Panel>
  );
}

// An import's provenance is what its uploader declared; the sandbox's
// validation is shown apart from it, and neither is called verified training.
function ImportProvenance({
  model,
  provenance,
}: {
  readonly model: ModelDetail;
  readonly provenance: Origin<'import'>;
}) {
  const { training } = model;
  const { declared, validation } = provenance;
  return (
    <Panel title="Training provenance" meta={ORIGIN_LABEL[provenance.origin]}>
      <Alert tone="caution" title="Declared by the uploader, not verified">
        <p>
          Chronos cannot check how this model was trained or on what data. Any evaluation of it is conditional on these
          declarations being true.
        </p>
      </Alert>
      <div className={styles.section}>
        <Facts
          facts={[
            { label: 'Information cutoff (UTC)', value: <CutoffText cutoff={training.information_cutoff} /> },
            {
              label: 'Declared to cover',
              value: declared.cutoff_covers ?? <Unavailable>Not declared</Unavailable>,
            },
          ]}
        />
        <CutoffMeaning model={model} />
      </div>
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Declared by the uploader</h3>
        <ul className={styles.list}>
          <li>
            <strong>Training.</strong> {sentence(declared.training)}
          </li>
          <li>
            <strong>Source.</strong> {declared.source === '' ? 'None given.' : sentence(declared.source)}
          </li>
        </ul>
        <Facts facts={[{ label: 'Manifest SHA-256', value: <Ids>{provenance.manifest_sha256}</Ids> }]} />
      </div>
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Sandbox validation</h3>
        <Facts
          facts={[
            {
              label: 'Profile',
              value: `${validation.profile.profile.name} ${validation.profile.profile.version}`,
            },
            {
              label: 'Operators',
              value: validation.profile.operators.join(', '),
            },
            {
              label: 'Nodes',
              value: <span className="numeric">{validation.profile.nodes}</span>,
            },
            {
              label: 'Opset',
              value: <span className="numeric">{validation.profile.opset}</span>,
            },
            { label: 'ONNX Runtime', value: validation.onnxruntime },
            {
              label: 'Smoke rows',
              value: <span className="numeric">{validation.smoke.rows}</span>,
            },
            {
              label: 'Smoke outputs SHA-256',
              value: <Ids>{validation.smoke.outputs_sha256}</Ids>,
            },
            {
              label: 'Sandbox source SHA-256',
              value: <Ids>{validation.sandbox_content_sha256}</Ids>,
            },
          ]}
        />
      </div>
      <Verified statements={model.verification} />
      <NotVerified
        statements={Object.entries(provenance.not_verified).map(
          ([key, text]) => [NOT_VERIFIED_LABEL[key] ?? key.replaceAll('_', ' '), text] as const,
        )}
      />
    </Panel>
  );
}

function Provenance({ model }: { readonly model: ModelDetail }) {
  return model.provenance.origin === 'import' ? (
    <ImportProvenance model={model} provenance={model.provenance} />
  ) : (
    <ExperimentProvenance model={model} provenance={model.provenance} />
  );
}

function Uses({ model }: { readonly model: ModelDetail }) {
  return (
    <Panel title="Inference compatibility">
      <div className={historyStyles.scroll}>
        <table className={historyStyles.table}>
          <caption className="visually-hidden">Where this version can be used</caption>
          <tbody className={styles.comparison}>
            <CompatibilityRow label="Historical evaluation" entry={model.compatibility.evaluation} />
            <CompatibilityRow label="Paper deployment" entry={model.compatibility.deployment} />
          </tbody>
        </table>
      </div>
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Limitations</h3>
        <ul className={styles.list}>
          {model.limitations.map((text) => (
            <li key={text}>{text}</li>
          ))}
        </ul>
      </div>
    </Panel>
  );
}

export function ModelDetailView({ modelId }: { readonly modelId: string }) {
  const model = useModel(modelId);
  if (model.status === 'pending') {
    return (
      <Panel title="Model version">
        <p role="status">Reading and verifying the model version…</p>
      </Panel>
    );
  }
  if (model.status === 'error') {
    return (
      <Panel title="Model version">
        <ResearchFailure
          error={model.error}
          title="The model version could not be read"
          onRetry={() => void model.refetch()}
          retrying={model.isFetching}
        />
      </Panel>
    );
  }
  const detail = model.data;
  return (
    <>
      {detail.lifecycle.state === 'archived' && (
        <Alert tone="caution" title="This version is archived">
          <p>It is kept for reference; new comparisons and deployments cannot choose it.</p>
        </Alert>
      )}
      <div className={styles.grid}>
        <div className={styles.column}>
          <Identity model={detail} />
          <Contract model={detail} />
        </div>
        <div className={styles.column}>
          <Provenance model={detail} />
          <Uses model={detail} />
          <DeployVersionForm model={detail} />
        </div>
      </div>
    </>
  );
}
