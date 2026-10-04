import { JOB_STATE, MODEL_LABEL, WINDOW_LABEL } from '../../presentation/research';
import { researchHref } from '../../state/route';
import { hasActiveJob, useRuns } from '../../state/research';
import historyStyles from '../history/history.module.css';
import { Badge } from '../ui/Badge';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ResearchFailure } from './Research';
import styles from './research.module.css';

// Evaluation jobs, newest first, as the research service reports them.
export function RunList() {
  const runs = useRuns();
  const active = hasActiveJob(runs.data);
  return (
    <Panel title="Runs" meta={active ? 'Updating while a run is active' : 'Newest first'}>
      {runs.status === 'pending' ? (
        <p role="status">Loading runs…</p>
      ) : runs.status === 'error' ? (
        <ResearchFailure error={runs.error} onRetry={() => void runs.refetch()} retrying={runs.isFetching} />
      ) : runs.data.runs.length === 0 ? (
        <p className={historyStyles.empty}>No evaluations have been started yet.</p>
      ) : (
        <div className={historyStyles.scroll}>
          <table className={historyStyles.table}>
            <caption className="visually-hidden">Evaluation runs, newest first</caption>
            <thead>
              <tr>
                <th scope="col">Requested (UTC)</th>
                <th scope="col">Evaluation</th>
                <th scope="col">State</th>
                <th scope="col">Outcome</th>
              </tr>
            </thead>
            <tbody>
              {runs.data.runs.map((job) => {
                const state = JOB_STATE[job.state];
                return (
                  <tr key={job.job_id}>
                    <td data-label="Requested (UTC)">
                      <MarketTime value={job.created_at} stacked />
                    </td>
                    <td data-label="Evaluation">
                      <div>
                        <span className="numeric">{job.request.instrument}</span> · {WINDOW_LABEL[job.request.window]}
                        <span className={styles.reasonCode}>{MODEL_LABEL[job.request.model]}</span>
                      </div>
                    </td>
                    <td data-label="State">
                      <Badge tone={state.tone}>{state.label}</Badge>
                    </td>
                    <td className={historyStyles.detail} data-label="Outcome">
                      <div>
                        {job.result !== null ? (
                          <a href={researchHref(job.result.run_id)}>Open result</a>
                        ) : job.error !== null ? (
                          <>
                            {job.error.message}
                            <span className={styles.reasonCode}>{job.error.code}</span>
                          </>
                        ) : (
                          <span className={styles.choiceDetail}>
                            {job.state === 'running' ? 'Verifying sources and simulating…' : 'Waiting for a worker'}
                          </span>
                        )}
                      </div>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </Panel>
  );
}
