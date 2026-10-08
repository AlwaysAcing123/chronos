import { JOB_STATE } from '../../presentation/research';
import { useModelJob } from '../../state/models';
import { comparisonsHref, modelsHref } from '../../state/route';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';

const WORDS = {
  registration: {
    noun: 'Registration',
    done: 'Model version registered',
    working: 'Verifying the experiment and its sources again before publishing…',
    again: 'This exact model version was already registered.',
  },
  import: {
    noun: 'Import',
    done: 'Model version imported',
    working: 'Checking the model against the profile and running the smoke corpus in the ONNX sandbox…',
    again: 'These exact bytes were already imported with this manifest.',
  },
  comparison: {
    noun: 'Comparison',
    done: 'Comparison published',
    working:
      'Verifying the split, predicting every member point in time, running and replaying each in its own accounts…',
    again: 'This exact comparison was already published.',
  },
} as const;

// The status of a job that publishes a model version, polled until it ends.
export function ModelJobStatus({ jobId, kind }: { readonly jobId: string; readonly kind: keyof typeof WORDS }) {
  const job = useModelJob(jobId);
  const words = WORDS[kind];
  if (job.data === undefined) {
    return job.status === 'error' ? (
      <ResearchFailure error={job.error} title={`The ${words.noun.toLowerCase()}'s status could not be read`} />
    ) : (
      <p role="status">Reading the {words.noun.toLowerCase()}…</p>
    );
  }
  const state = JOB_STATE[job.data.state];
  const { result, error } = job.data;
  return (
    <Alert
      tone={state.tone}
      title={job.data.state === 'succeeded' ? words.done : `${words.noun} ${state.label.toLowerCase()}`}
      live
    >
      <p>
        <Badge tone={state.tone}>{state.label}</Badge>{' '}
        {job.data.state === 'queued' || job.data.state === 'running' ? words.working : null}
      </p>
      {result !== null && (
        <p>
          {result.outcome === 'already_present' ? `${words.again} ` : ''}
          {'comparison_id' in result ? (
            <a href={comparisonsHref(result.comparison_id)}>Open the comparison</a>
          ) : (
            <a href={modelsHref(result.model_version_id)}>Open the model version</a>
          )}
        </p>
      )}
      {error !== null && (
        <p>
          {error.message}
          <span className={styles.reasonCode}>{error.reason ?? error.code}</span>
        </p>
      )}
    </Alert>
  );
}
