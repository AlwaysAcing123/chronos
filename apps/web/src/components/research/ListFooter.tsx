import historyStyles from '../history/history.module.css';
import { Button } from '../ui/Button';

// The end of a paged list: how many entries are shown and, when the
// service has older ones, an explicit way to load the next page. Nothing
// is fetched until asked, and a failed page keeps what is shown.
export function ListFooter({
  shown,
  noun,
  hasMore,
  loading,
  failed,
  onLoadMore,
}: {
  readonly shown: number;
  readonly noun: string;
  readonly hasMore: boolean;
  readonly loading: boolean;
  readonly failed: boolean;
  readonly onLoadMore: () => void;
}) {
  if (!hasMore && !failed) {
    return null;
  }
  return (
    <div className={historyStyles.footer}>
      <span>
        {shown} {noun} shown
      </span>
      {failed && <span role="status">The next page could not be loaded.</span>}
      {(hasMore || failed) && (
        <Button onClick={onLoadMore} disabled={loading}>
          {loading ? 'Loading…' : failed ? 'Try again' : `Load more ${noun}`}
        </Button>
      )}
    </div>
  );
}
