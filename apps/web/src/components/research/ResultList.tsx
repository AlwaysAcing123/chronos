import { MODEL_LABEL, WINDOW_LABEL, shortId } from '../../presentation/research';
import { researchHref } from '../../state/route';
import { useResults } from '../../state/research';
import historyStyles from '../history/history.module.css';
import { Alert, Code } from '../ui/Alert';
import { DecimalText, Unavailable } from '../ui/DecimalText';
import { Panel } from '../ui/Panel';
import { ListFooter } from './ListFooter';
import { ResearchFailure } from './Research';
import styles from './research.module.css';

// Every published result, found again after a reload or a restart.
export function ResultList() {
  const results = useResults();
  return (
    <Panel title="Saved results" meta="Immutable, verified when read">
      {results.status === 'pending' ? (
        <p role="status">Loading results…</p>
      ) : results.data === undefined ? (
        <ResearchFailure
          error={results.error}
          onRetry={() => void results.refetch()}
          retrying={results.isFetching}
        />
      ) : (
        <>
          {results.data.invalid_entries.length > 0 && (
            <Alert tone="caution" title="Some results cannot be read">
              <ul className={styles.list}>
                {results.data.invalid_entries.map((entry) => (
                  <li key={entry.run_id}>
                    <Code>{shortId(entry.run_id)}</Code> {entry.reason}
                  </li>
                ))}
              </ul>
            </Alert>
          )}
          {results.data.results.length === 0 ? (
            <p className={historyStyles.empty}>No results have been published yet.</p>
          ) : (
            <div className={historyStyles.scroll}>
              <table className={historyStyles.table}>
                <caption className="visually-hidden">Saved evaluation results</caption>
                <thead>
                  <tr>
                    <th scope="col">Evaluation</th>
                    <th scope="col">Model</th>
                    <th scope="col" className={historyStyles.number}>
                      Strategy net PnL
                    </th>
                    <th scope="col">Result</th>
                  </tr>
                </thead>
                <tbody>
                  {results.data.results.map((result) => (
                    <tr key={result.run_id}>
                      <td data-label="Evaluation">
                        <div>
                          <span className="numeric">{result.instrument}</span> · {WINDOW_LABEL[result.window.name]}
                        </div>
                      </td>
                      <td data-label="Model">{MODEL_LABEL[result.model]}</td>
                      <td className={historyStyles.number} data-label="Strategy net PnL">
                        {result.net_total_pnl === null ? (
                          <Unavailable>Valuation unavailable</Unavailable>
                        ) : (
                          <DecimalText value={result.net_total_pnl} signed />
                        )}
                      </td>
                      <td data-label="Result">
                        <a href={researchHref(result.run_id)} aria-label={`Open result ${shortId(result.run_id)}`}>
                          <span className="mono">{shortId(result.run_id)}</span>
                        </a>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
          <ListFooter
            shown={results.data.results.length}
            noun="results"
            hasMore={results.hasNextPage}
            loading={results.isFetchingNextPage}
            failed={results.isFetchNextPageError}
            onLoadMore={() => void results.fetchNextPage()}
          />
        </>
      )}
    </Panel>
  );
}
