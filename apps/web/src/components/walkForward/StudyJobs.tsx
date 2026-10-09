import type { StudyJob } from '../../api/walkForwardSchemas';
import { JOB_STATE, shortId } from '../../presentation/research';
import { STUDY_STATUS, progressText, studyReason } from '../../presentation/walkForward';
import { walkForwardHref } from '../../state/route';
import { useStudyJobs, useWalkForwardJob } from '../../state/walkForward';
import historyStyles from '../history/history.module.css';
import { ListFooter } from '../research/ListFooter';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';

function Outcome({ job }: { readonly job: StudyJob }) {
  if (job.result !== null) {
    const status = STUDY_STATUS[job.result.status];
    return (
      <>
        <Badge tone={status.tone}>{status.label}</Badge>{' '}
        {job.result.missing !== '0' && `${job.result.missing} items missing. `}
        <a href={walkForwardHref({ studyId: job.result.study_id })}>Open the study</a>
      </>
    );
  }
  if (job.error !== null) {
    return (
      <>
        {job.error.reason === undefined ? job.error.message : studyReason(job.error.reason)}
        <span className={styles.reasonCode}>{job.error.reason ?? job.error.code}</span>
      </>
    );
  }
  return <>{progressText(job.progress)}</>;
}

// A started study, followed until it ends: its latest stage and fold while
// it runs, then its study or why it has none.
export function StudyJobStatus({ jobId }: { readonly jobId: string }) {
  const job = useWalkForwardJob(jobId);
  if (job.data === undefined) {
    return job.status === 'error' ? (
      <ResearchFailure error={job.error} title="The study’s status could not be read" />
    ) : (
      <p role="status">Reading the study job…</p>
    );
  }
  if (job.data.kind !== 'walk_forward') {
    return null;
  }
  const state = JOB_STATE[job.data.state];
  const title =
    job.data.state === 'succeeded'
      ? job.data.result?.status === 'partial'
        ? 'Study published, partial'
        : 'Study published'
      : `Study ${state.label.toLowerCase()}`;
  return (
    <Alert tone={job.data.state === 'succeeded' && job.data.result?.status === 'partial' ? 'caution' : state.tone} title={title} live>
      <p>
        <Badge tone={state.tone}>{state.label}</Badge> <Outcome job={job.data} />
      </p>
      {job.data.result?.outcome === 'already_present' && <p>This exact study was already published.</p>}
      {job.data.state === 'interrupted' && (
        <p>
          It stopped at: {progressText(job.data.progress)}. It never resumes by itself; start the plan again for a
          complete study.
        </p>
      )}
    </Alert>
  );
}

// Every walk-forward study job, newest first, polled while any is active.
export function StudyJobs() {
  const jobs = useStudyJobs();
  return (
    <Panel title="Study jobs" meta="Newest first">
      {jobs.status === 'pending' ? (
        <p role="status">Loading study jobs…</p>
      ) : jobs.data === undefined ? (
        <ResearchFailure
          error={jobs.error}
          title="The study jobs could not be read"
          onRetry={() => void jobs.refetch()}
          retrying={jobs.isFetching}
        />
      ) : jobs.data.jobs.length === 0 ? (
        <div className={historyStyles.empty}>
          <p>No study has been started.</p>
        </div>
      ) : (
        <div className={historyStyles.scroll}>
          <table className={historyStyles.table}>
            <caption className="visually-hidden">Walk-forward study jobs, newest first</caption>
            <thead>
              <tr>
                <th scope="col">Started</th>
                <th scope="col">Plan</th>
                <th scope="col">State</th>
                <th scope="col">Progress or outcome</th>
              </tr>
            </thead>
            <tbody>
              {jobs.data.jobs.map((job) => (
                <tr key={job.job_id}>
                  <td data-label="Started">
                    <MarketTime value={job.created_at} stacked />
                  </td>
                  <td data-label="Plan">
                    <a href={walkForwardHref({ planId: job.request.plan_id })}>{shortId(job.request.plan_id)}</a>
                  </td>
                  <td data-label="State">
                    <Badge tone={JOB_STATE[job.state].tone}>{JOB_STATE[job.state].label}</Badge>
                  </td>
                  <td data-label="Progress or outcome" className={historyStyles.detail}>
                    <span>
                      <Outcome job={job} />
                    </span>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          <ListFooter
            shown={jobs.data.jobs.length}
            noun="jobs"
            hasMore={jobs.hasNextPage}
            loading={jobs.isFetchingNextPage}
            failed={jobs.isFetchNextPageError}
            onLoadMore={() => void jobs.fetchNextPage()}
          />
        </div>
      )}
    </Panel>
  );
}
