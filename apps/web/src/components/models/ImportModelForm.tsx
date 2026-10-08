import { useId, useState } from 'react';

import type { ImportManifest } from '../../api/modelSchemas';
import { ImportManifestSchema } from '../../api/modelSchemas';
import type { ResearchStatus } from '../../api/researchSchemas';
import { CUTOFF_BASIS, byteSize, sentence, timestampOfNanoseconds } from '../../presentation/models';
import type { ReviewedImport } from '../../state/models';
import { ImportStepError, useImportModel } from '../../state/models';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { Unavailable } from '../ui/DecimalText';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ModelJobStatus } from './ModelJobStatus';

// The research service's own limit for a JSON body.
const MAX_MANIFEST_BYTES = 16 * 1024;

const STEP_TITLE = {
  stage: 'The manifest was not accepted',
  upload: 'The model file was not accepted',
  validate: 'Validation could not start',
} as const;

interface Reviewed extends ReviewedImport {
  readonly manifest: ImportManifest;
  readonly modelName: string;
}

async function sha256(bytes: Uint8Array<ArrayBuffer>): Promise<string> {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return Array.from(digest, (byte) => byte.toString(16).padStart(2, '0')).join('');
}

// Reads both files once, in the browser, so what is shown is what is sent.
async function readFiles(manifestFile: File, modelFile: File, maxModelBytes: number): Promise<Reviewed> {
  if (manifestFile.size > MAX_MANIFEST_BYTES) {
    throw new Error(`the manifest is larger than ${byteSize(MAX_MANIFEST_BYTES)}`);
  }
  if (modelFile.size === 0 || modelFile.size > maxModelBytes) {
    throw new Error(`the model file must be 1 byte to ${byteSize(maxModelBytes)}`);
  }
  const manifestText = await manifestFile.text();
  let document: unknown;
  try {
    document = JSON.parse(manifestText);
  } catch {
    throw new Error('the manifest is not JSON');
  }
  const manifest = ImportManifestSchema.safeParse(document);
  if (!manifest.success) {
    const issue = manifest.error.issues[0];
    throw new Error(`the manifest is not a chronos.model_import/1 manifest (${issue?.path.join('.') ?? 'document'})`);
  }
  const model = new Uint8Array(await modelFile.arrayBuffer());
  return {
    manifestText,
    manifest: manifest.data,
    model,
    modelSha256: await sha256(model),
    modelName: modelFile.name,
  };
}

function DeclaredCutoff({ manifest }: { readonly manifest: ImportManifest }) {
  const declared = manifest.declared.information_cutoff_ns;
  const at = declared === null ? null : timestampOfNanoseconds(declared);
  const basis = CUTOFF_BASIS[declared === null ? 'unavailable' : 'declared'];
  return (
    <div>
      {at === null ? <Unavailable>Not declared</Unavailable> : <MarketTime value={at} stacked />}
      <Badge tone={basis.tone}>{basis.label}</Badge>
    </div>
  );
}

function Review({
  reviewed,
  sandbox,
}: {
  readonly reviewed: Reviewed;
  readonly sandbox: ResearchStatus['imports']['sandbox'];
}) {
  const { manifest } = reviewed;
  return (
    <>
      <Facts
        facts={[
          {
            label: 'Name',
            value: <span className="mono">{manifest.name}</span>,
          },
          {
            label: 'Version',
            value: <span className="mono">{manifest.version}</span>,
          },
          { label: 'Instrument', value: manifest.instrument },
          {
            label: 'Inputs',
            value: `${manifest.inputs.name} version ${manifest.inputs.version}`,
          },
          {
            label: 'Tensors',
            value: `${manifest.input_tensor.name} ${manifest.input_tensor.dtype} [${manifest.input_tensor.shape.join(', ')}] → ${manifest.output_tensor.name} ${manifest.output_tensor.dtype} [${manifest.output_tensor.shape.join(', ')}]`,
          },
          {
            label: 'Classes, in order',
            value: manifest.class_order.join(', '),
          },
          {
            label: 'Target',
            value: `${manifest.target.label_set}, horizon ${manifest.target.horizon_ns} ns`,
          },
          {
            label: 'Information cutoff (UTC)',
            value: <DeclaredCutoff manifest={manifest} />,
          },
          { label: 'Model file', value: reviewed.modelName },
          {
            label: 'Model size (bytes)',
            value: <span className="numeric">{reviewed.model.byteLength}</span>,
          },
          {
            label: 'Model SHA-256',
            value: <span className={styles.ids}>{reviewed.modelSha256}</span>,
          },
        ]}
      />
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Declared by the uploader, not verified</h3>
        <ul className={styles.list}>
          <li>
            <strong>Training.</strong> {sentence(manifest.declared.training)}
          </li>
          <li>
            <strong>Source.</strong>{' '}
            {manifest.declared.source === '' ? 'None given.' : sentence(manifest.declared.source)}
          </li>
          <li>
            <strong>Cutoff covers.</strong>{' '}
            {manifest.declared.cutoff_covers === null ? 'Not declared.' : sentence(manifest.declared.cutoff_covers)}
          </li>
        </ul>
      </div>
      <p className={styles.note}>
        The service stores these exact bytes, then checks them against the profile and runs a fixed smoke corpus in the
        ONNX sandbox: no network, nothing of this computer mounted, and bounded memory, CPU, and time. Only if every
        check passes are the same bytes published as a model version.
      </p>
      {sandbox.status === 'unavailable' && (
        <Alert tone="caution" title="The ONNX sandbox is unavailable">
          <p>
            {sentence(sandbox.detail)} Nothing is validated any other way.
            <span className={styles.reasonCode}>{sandbox.reason}</span>
          </p>
        </Alert>
      )}
    </>
  );
}

// Imports an ONNX classifier: a chronos.model_import/1 manifest and a model
// file, read in the browser and reviewed with the model's SHA-256 before
// anything is sent.
export function ImportModelForm({ status }: { readonly status: ResearchStatus }) {
  const formId = useId();
  const { profile, sandbox } = status.imports;
  const maxModelBytes = Number(profile.max_model_bytes);
  const [manifestFile, setManifestFile] = useState<File | null>(null);
  const [modelFile, setModelFile] = useState<File | null>(null);
  const [reviewed, setReviewed] = useState<Reviewed | null>(null);
  const [readError, setReadError] = useState<string | null>(null);
  const [reading, setReading] = useState(false);
  const importing = useImportModel();
  const failure = importing.error instanceof ImportStepError ? importing.error : null;

  if (reviewed !== null) {
    return (
      <Panel title="Import an ONNX model" meta="Review">
        <div className={styles.review}>
          <Review reviewed={reviewed} sandbox={sandbox} />
          {importing.data !== undefined ? (
            <ModelJobStatus jobId={importing.data.job.job_id} kind="import" />
          ) : (
            failure !== null && <ResearchFailure error={failure.cause} title={STEP_TITLE[failure.step]} />
          )}
          <div className={styles.actions}>
            {importing.data === undefined && (
              <Button
                variant="primary"
                disabled={importing.isPending || sandbox.status !== 'available'}
                onClick={() => {
                  importing.mutate(reviewed);
                }}
              >
                {importing.isPending ? 'Sending…' : 'Validate and import'}
              </Button>
            )}
            <Button
              onClick={() => {
                setReviewed(null);
                importing.reset();
              }}
            >
              {importing.data === undefined ? 'Choose other files' : 'Import another'}
            </Button>
          </div>
        </div>
      </Panel>
    );
  }

  return (
    <Panel title="Import an ONNX model" meta={`${profile.profile.name} ${profile.profile.version}`}>
      <form
        className={styles.review}
        aria-describedby={`${formId}-note`}
        onSubmit={(event) => {
          event.preventDefault();
          if (manifestFile === null || modelFile === null) {
            return;
          }
          setReading(true);
          setReadError(null);
          readFiles(manifestFile, modelFile, maxModelBytes)
            .then(setReviewed)
            .catch((error: unknown) => {
              setReadError(error instanceof Error ? error.message : 'the files could not be read');
            })
            .finally(() => {
              setReading(false);
            });
        }}
      >
        <div className={styles.fields}>
          <div className={styles.field}>
            <label htmlFor={`${formId}-manifest`}>Import manifest (JSON)</label>
            <input
              id={`${formId}-manifest`}
              type="file"
              accept=".json,application/json"
              onChange={(event) => {
                setManifestFile(event.target.files?.[0] ?? null);
              }}
            />
          </div>
          <div className={styles.field}>
            <label htmlFor={`${formId}-model`}>Model (.onnx)</label>
            <input
              id={`${formId}-model`}
              type="file"
              accept=".onnx"
              aria-describedby={`${formId}-model-hint`}
              onChange={(event) => {
                setModelFile(event.target.files?.[0] ?? null);
              }}
            />
            <span id={`${formId}-model-hint`} className={styles.fieldHint}>
              At most {byteSize(maxModelBytes)}
            </span>
          </div>
        </div>
        <p id={`${formId}-note`} className={styles.note}>
          Supported: one double input [1, 5] of the top_of_book_baseline version 1 inputs, three double probabilities
          [1, 3] for down, flat, and up, from the operators {profile.operators.join(', ')} at opsets{' '}
          {profile.opsets.join(' to ')}. No external data, custom operators, control flow, or randomness.
        </p>
        {readError !== null && (
          <Alert tone="negative" title="The files could not be reviewed">
            <p>{sentence(readError)}</p>
          </Alert>
        )}
        <div className={styles.actions}>
          <Button type="submit" variant="primary" disabled={manifestFile === null || modelFile === null || reading}>
            {reading ? 'Reading…' : 'Review import'}
          </Button>
        </div>
      </form>
    </Panel>
  );
}
