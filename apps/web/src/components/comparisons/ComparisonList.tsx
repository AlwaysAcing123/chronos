import { MEMBER_STATE } from '../../presentation/comparisons';
import { modelName } from '../../presentation/models';
import { WINDOW_LABEL, shortId } from '../../presentation/research';
import { useComparisons } from '../../state/comparisons';
import { comparisonsHref } from '../../state/route';
import historyStyles from '../history/history.module.css';
import { ListFooter } from '../research/ListFooter';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';

// Every published comparison, each verified when listed.
export function ComparisonList() {
  const comparisons = useComparisons();
  return (
    <Panel title="Published comparisons" meta="Verified when read">
      {comparisons.status === 'pending' ? (
        <p role="status">Loading comparisons…</p>
      ) : comparisons.data === undefined ? (
        <ResearchFailure
          error={comparisons.error}
          title="The comparisons could not be read"
          onRetry={() => void comparisons.refetch()}
          retrying={comparisons.isFetching}
        />
      ) : (
        <>
          {comparisons.data.invalid_entries.length > 0 && (
            <Alert tone="negative" title="Some comparisons do not verify and are not shown">
              <ul className={styles.list}>
                {comparisons.data.invalid_entries.map((entry) => (
                  <li key={entry.comparison_id}>
                    <Code>{shortId(entry.comparison_id)}</Code> {entry.reason}
                  </li>
                ))}
              </ul>
            </Alert>
          )}
          {comparisons.data.comparisons.length === 0 ? (
            <div className={historyStyles.empty}>
              <p>No comparisons are published yet.</p>
              <p className={styles.note}>Choose two or more model versions with the form beside.</p>
            </div>
          ) : (
            <div className={historyStyles.scroll}>
              <table className={historyStyles.table}>
                <caption className="visually-hidden">Published comparisons</caption>
                <thead>
                  <tr>
                    <th scope="col">Comparison</th>
                    <th scope="col">Window</th>
                    <th scope="col">Members</th>
                  </tr>
                </thead>
                <tbody>
                  {comparisons.data.comparisons.map((entry) => (
                    <tr key={entry.comparison_id}>
                      <td data-label="Comparison">
                        <a href={comparisonsHref(entry.comparison_id)}>{shortId(entry.comparison_id)}</a>
                        <span className={styles.reasonCode}>{entry.instrument}</span>
                      </td>
                      <td data-label="Window">
                        {WINDOW_LABEL[entry.window.name]}
                        <MarketTime value={entry.window.start_at} stacked />
                      </td>
                      <td data-label="Members">
                        <ul className={styles.list}>
                          {entry.members.map((member) => (
                            <li key={member.model_version_id}>
                              {modelName(member)}{' '}
                              <Badge tone={MEMBER_STATE[member.state].tone}>{MEMBER_STATE[member.state].label}</Badge>
                            </li>
                          ))}
                        </ul>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
              <ListFooter
                shown={comparisons.data.comparisons.length}
                noun="comparisons"
                hasMore={comparisons.hasNextPage}
                loading={comparisons.isFetchingNextPage}
                failed={comparisons.isFetchNextPageError}
                onLoadMore={() => void comparisons.fetchNextPage()}
              />
            </div>
          )}
        </>
      )}
    </Panel>
  );
}
