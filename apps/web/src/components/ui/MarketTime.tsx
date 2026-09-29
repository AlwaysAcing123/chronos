import type { Timestamp } from '../../api/schemas';
import { formatMarketClock, formatMarketDate, formatMarketTime } from '../../presentation/format';
import styles from './ui.module.css';

// A UTC market timestamp, exact to the nanosecond. `stacked` puts the date
// above the time to keep table columns narrow.
export function MarketTime({ value, stacked = false }: { readonly value: Timestamp; readonly stacked?: boolean }) {
  return stacked ? (
    <time dateTime={value} className={styles.stackedTime}>
      <span className={styles.stackedDate}>{formatMarketDate(value)}</span>
      <span className="mono">{formatMarketClock(value)}</span>
    </time>
  ) : (
    <time dateTime={value} className="mono">
      {formatMarketTime(value)}
    </time>
  );
}
