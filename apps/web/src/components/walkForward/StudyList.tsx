import { shortId } from '../../presentation/research';
import { EVIDENCE, STUDY_STATUS } from '../../presentation/walkForward';
import { walkForwardHref } from '../../state/route';
import { useStudies } from '../../state/walkForward';
import historyStyles from '../history/history.module.css';
import { ListFooter } from '../research/ListFooter';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Panel } from '../ui/Panel';

// Every published study, each verified when listed.
export function StudyList() {
  const studies = useStudies();
  return (
    <Panel title="Published studies" meta="Verified when read">
      {studies.status === 'pending' ? (
        <p role="status">Loading studies…</p>
      ) : studies.data === undefined ? (
        <ResearchFailure
          error={studies.error}
          title="The studies could not be read"
          onRetry={() => void studies.refetch()}
          retrying={studies.isFetching}
        />
      ) : (
        <>
          {studies.data.invalid_entries.length > 0 && (
            <Alert tone="negative" title="Some studies do not verify and are not shown">
              <ul className={styles.list}>
                {studies.data.invalid_entries.map((entry) => (
                  <li key={entry.study_id}>
                    <Code>{shortId(entry.study_id)}</Code> {entry.reason}
                  </li>
                ))}
              </ul>
            </Alert>
          )}
          {studies.data.studies.length === 0 ? (
            <div className={historyStyles.empty}>
              <p>No walk-forward study is published yet.</p>
              <p className={styles.note}>Plan one with the form, review it, and start it.</p>
            </div>
          ) : (
            <div className={historyStyles.scroll}>
              <table className={historyStyles.table}>
                <caption className="visually-hidden">Published walk-forward studies</caption>
                <thead>
                  <tr>
                    <th scope="col">Study</th>
                    <th scope="col">Recordings</th>
                    <th scope="col">Folds</th>
                    <th scope="col">Status</th>
                  </tr>
                </thead>
                <tbody>
                  {studies.data.studies.map((entry) => (
                    <tr key={entry.study_id}>
                      <td data-label="Study">
                        <span>
                          <a href={walkForwardHref({ studyId: entry.study_id })}>{shortId(entry.study_id)}</a>
                          <span className={styles.reasonCode}>{entry.instrument}</span>
                        </span>
                      </td>
                      <td data-label="Recordings">
                        <Badge tone={EVIDENCE[entry.evidence].tone}>{EVIDENCE[entry.evidence].label}</Badge>
                      </td>
                      <td data-label="Folds" className={historyStyles.number}>
                        {entry.folds}
                      </td>
                      <td data-label="Status">
                        <span>
                          <Badge tone={STUDY_STATUS[entry.status].tone}>{STUDY_STATUS[entry.status].label}</Badge>
                          {entry.missing !== '0' && <span className={styles.reasonCode}>{entry.missing} items missing</span>}
                        </span>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
              <ListFooter
                shown={studies.data.studies.length}
                noun="studies"
                hasMore={studies.hasNextPage}
                loading={studies.isFetchingNextPage}
                failed={studies.isFetchNextPageError}
                onLoadMore={() => void studies.fetchNextPage()}
              />
            </div>
          )}
        </>
      )}
    </Panel>
  );
}
