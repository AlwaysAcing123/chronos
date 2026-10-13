import { useState } from 'react';

import type { DiagnosticsJob } from '../../api/diagnosticsSchemas';
import { STAGE, diagnosticReason } from '../../presentation/diagnostics';
import { JOB_STATE, MODEL_LABEL, shortId } from '../../presentation/research';
import { scenarioLabel } from '../../presentation/walkForward';
import { useDiagnosticsJob, useDiagnosticsList, useStartDiagnostics } from '../../state/diagnostics';
import { diagnosticsHref } from '../../state/route';
import historyStyles from '../history/history.module.css';
import { ListFooter } from '../research/ListFooter';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { Panel } from '../ui/Panel';

function progressText(job: DiagnosticsJob): string {
  const progress = job.progress;
  if (progress === null) {
    return job.state === 'queued' ? 'Waiting for a worker' : 'Starting';
  }
  const stage = STAGE[progress.stage] ?? progress.stage;
  if (progress.fold === undefined) {
    return stage;
  }
  const model = progress.model === undefined ? '' : `, ${MODEL_LABEL[progress.model]}`;
  const scenario = progress.scenario === undefined ? '' : `, ${scenarioLabel(progress.scenario)}`;
  return `${stage}: fold ${progress.fold} of ${progress.folds ?? '?'}${model}${scenario}`;
}

// A diagnostics job, followed until it ends.
function DiagnosticsJobStatus({ jobId, deduplicated }: { readonly jobId: string; readonly deduplicated: boolean }) {
  const job = useDiagnosticsJob(jobId);
  if (job.data === undefined) {
    return job.status === 'error' ? (
      <ResearchFailure error={job.error} title="The diagnostics job could not be read" />
    ) : (
      <p role="status">Reading the diagnostics job…</p>
    );
  }
  const state = JOB_STATE[job.data.state];
  const result = job.data.result;
  return (
    <Alert
      tone={state.tone}
      title={job.data.state === 'succeeded' ? 'Diagnostics published' : `Diagnostics ${state.label.toLowerCase()}`}
      live
    >
      <p>
        <Badge tone={state.tone}>{state.label}</Badge>{' '}
        {result !== null ? (
          <a href={diagnosticsHref({ diagnosticId: result.diagnostic_id })}>Open the diagnostics</a>
        ) : job.data.error !== null ? (
          <>
            {job.data.error.reason === undefined ? job.data.error.message : diagnosticReason(job.data.error.reason)}{' '}
            <span className={styles.reasonCode}>{job.data.error.reason ?? job.data.error.code}</span>
          </>
        ) : (
          progressText(job.data)
        )}
      </p>
      {deduplicated && <p>The same diagnostics were already requested; this is that job.</p>}
      {result?.outcome === 'already_present' && <p>These exact diagnostics were already published.</p>}
      {job.data.state === 'interrupted' && (
        <p>It stopped at: {progressText(job.data)}. It never resumes by itself; request it again.</p>
      )}
    </Alert>
  );
}

// The diagnostics that explain a study, and the one way to generate them: a
// job that reads the study's runs and recordings, verifies them, and fits,
// schedules, and runs nothing.
export function StudyDiagnostics({ studyId }: { readonly studyId: string }) {
  const list = useDiagnosticsList(studyId);
  const start = useStartDiagnostics();
  const [started, setStarted] = useState<{ readonly jobId: string; readonly deduplicated: boolean } | null>(null);
  return (
    <Panel title="Strategy diagnostics" meta="Why each run made or lost money">
      <p className={styles.note}>
        Diagnostics split every run’s exact net PnL into spread paid, midpoint movement after the signal, fees, the final
        mark at the bid, and rounding, and describe the decisions behind it. They are built from this study’s own
        journals and recordings; nothing is fitted, scheduled, or run again.
      </p>
      {list.status === 'pending' ? (
        <p role="status">Looking for diagnostics of this study…</p>
      ) : list.data === undefined ? (
        <ResearchFailure
          error={list.error}
          title="The diagnostics could not be listed"
          onRetry={() => void list.refetch()}
          retrying={list.isFetching}
        />
      ) : (
        <>
          {list.data.invalid_entries.length > 0 && (
            <Alert tone="negative" title="Some diagnostics do not verify">
              <ul className={styles.list}>
                {list.data.invalid_entries.map((entry) => (
                  <li key={entry.diagnostic_id}>
                    {shortId(entry.diagnostic_id)}: {entry.reason}
                  </li>
                ))}
              </ul>
            </Alert>
          )}
          {list.data.diagnostics.length === 0 ? (
            <div className={historyStyles.empty}>
              <p>No diagnostics of this study exist yet.</p>
            </div>
          ) : (
            <ul className={styles.list}>
              {list.data.diagnostics.map((entry) => (
                <li key={entry.diagnostic_id}>
                  <a href={diagnosticsHref({ diagnosticId: entry.diagnostic_id })}>
                    Diagnostics {shortId(entry.diagnostic_id)}
                  </a>{' '}
                  of {entry.studies.map((study) => study.instrument).join(' and ')}:{' '}
                  {entry.counts.runs_attributed} of {entry.counts.runs} runs, {entry.counts.fills} fills{' '}
                  <Badge tone={entry.status === 'complete' ? 'positive' : 'caution'}>
                    {entry.status === 'complete' ? 'Complete' : 'Partial'}
                  </Badge>
                </li>
              ))}
            </ul>
          )}
          <ListFooter
            shown={list.data.diagnostics.length}
            noun="diagnostics"
            hasMore={list.hasNextPage}
            loading={list.isFetchingNextPage}
            failed={list.isFetchNextPageError}
            onLoadMore={() => void list.fetchNextPage()}
          />
        </>
      )}
      <div className={styles.actions}>
        <Button
          variant="primary"
          disabled={start.isPending}
          onClick={() => {
            start.mutate([studyId], {
              onSuccess: (outcome) => {
                setStarted({ jobId: outcome.job.job_id, deduplicated: outcome.deduplicated });
              },
            });
          }}
        >
          {start.isPending ? 'Requesting…' : 'Explain this study’s runs'}
        </Button>
      </div>
      {start.isError && <ResearchFailure error={start.error} title="The diagnostics could not be requested" />}
      {started !== null && <DiagnosticsJobStatus jobId={started.jobId} deduplicated={started.deduplicated} />}
    </Panel>
  );
}
