import type { ReactNode } from 'react';

import type { Side } from '../../api/schemas';
import { SIDE } from '../../presentation/vocabulary';
import { Alert } from '../ui/Alert';
import { Button } from '../ui/Button';
import { cx } from '../ui/cx';
import { FailureNotice } from '../ui/FailureNotice';
import uiStyles from '../ui/ui.module.css';
import styles from './history.module.css';

interface HistoryQuery {
  readonly status: 'pending' | 'error' | 'success';
  readonly error: unknown;
  readonly isFetching: boolean;
  readonly hasNextPage: boolean;
  readonly isFetchingNextPage: boolean;
  readonly isFetchNextPageError: boolean;
  readonly isRefetchError: boolean;
  refetch(): Promise<unknown>;
  fetchNextPage(): Promise<unknown>;
}

// Loading, failure, empty, and paging states shared by both history lists.
export function HistoryView({
  query,
  itemCount,
  noun,
  emptyText,
  children,
}: {
  readonly query: HistoryQuery;
  readonly itemCount: number;
  readonly noun: string;
  readonly emptyText: string;
  readonly children: ReactNode;
}) {
  if (query.status === 'pending') {
    return (
      <div className={styles.skeletonRows} aria-label={`Loading ${noun}`} role="status">
        <span className={uiStyles.skeleton} />
        <span className={uiStyles.skeleton} />
        <span className={uiStyles.skeleton} />
      </div>
    );
  }
  if (query.status === 'error' && !query.isRefetchError) {
    return <FailureNotice error={query.error} onRetry={() => void query.refetch()} retrying={query.isFetching} />;
  }
  return (
    <div className={styles.stack}>
      {query.isRefetchError && (
        // The journal is append-only, so rows already read remain true.
        <Alert
          tone="caution"
          title={`Refreshing ${noun} failed`}
          actions={
            <Button onClick={() => void query.refetch()} disabled={query.isFetching}>
              Retry
            </Button>
          }
        >
          Showing the {noun} read before the failure; recorded orders never change.
        </Alert>
      )}
      {itemCount === 0 ? <p className={styles.empty}>{emptyText}</p> : children}
      {itemCount > 0 && (
        <div className={styles.footer}>
          <span>
            {itemCount} {noun} loaded · newest first
          </span>
          {query.hasNextPage ? (
            <Button onClick={() => void query.fetchNextPage()} disabled={query.isFetchingNextPage}>
              {query.isFetchingNextPage ? 'Loading…' : `Load older ${noun}`}
            </Button>
          ) : (
            <span>Oldest reached</span>
          )}
          {query.isFetchNextPageError && <span role="status">Loading older {noun} failed. Try again.</span>}
        </div>
      )}
    </div>
  );
}

export function SideText({ side }: { readonly side: Side }) {
  return <span className={styles.side}>{SIDE[side]}</span>;
}

export interface Column<Row> {
  readonly header: string;
  readonly numeric?: boolean;
  // A free-text column that may wrap.
  readonly wraps?: boolean;
  readonly cell: (row: Row) => ReactNode;
}

// Explicit table roles keep the semantics when narrow screens restyle rows
// as labelled cards.
export function HistoryTable<Row>({
  label,
  caption,
  columns,
  rows,
  rowKey,
}: {
  readonly label: string;
  readonly caption: string;
  readonly columns: readonly Column<Row>[];
  readonly rows: readonly Row[];
  readonly rowKey: (row: Row) => string;
}) {
  return (
    // Focusable so keyboard users can scroll a table wider than the screen.
    <div className={styles.scroll} role="region" aria-label={label} tabIndex={0}>
      <table className={styles.table} role="table">
        <caption className="visually-hidden">{caption}</caption>
        <thead role="rowgroup">
          <tr role="row">
            {columns.map((column) => (
              <th key={column.header} scope="col" role="columnheader" className={cx(column.numeric && styles.number)}>
                {column.header}
              </th>
            ))}
          </tr>
        </thead>
        <tbody role="rowgroup">
          {rows.map((row) => (
            <tr key={rowKey(row)} role="row">
              {columns.map((column) => (
                <td
                  key={column.header}
                  role="cell"
                  data-label={column.header}
                  className={cx(column.numeric && styles.number, column.wraps && styles.detail)}
                >
                  <span>{column.cell(row)}</span>
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
