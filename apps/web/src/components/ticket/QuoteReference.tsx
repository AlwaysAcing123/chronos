import type { PortfolioReading } from '../../api/client';
import { formatMarketClock } from '../../presentation/format';
import { Alert } from '../ui/Alert';
import { ReasonText } from '../ui/ReasonText';
import styles from './ticket.module.css';

// The quote the portfolio was last valued against, for reference only: the
// server executes against its own quote at the moment of submission.
export function QuoteReference({ reading }: { readonly reading: PortfolioReading | null }) {
  if (reading === null) {
    return null;
  }
  if (reading.kind === 'unavailable') {
    return (
      <Alert tone="caution" title="No usable quote right now">
        <ReasonText error={reading.error} />. The server checks again when the order is submitted.
      </Alert>
    );
  }
  const market = reading.portfolio.market;
  return (
    <div className={styles.quote} role="group" aria-label="Reference quote">
      {(['bid', 'ask'] as const).map((side) => {
        const level = market[side];
        return (
          <div key={side} className={styles.quoteSide}>
            <div className={styles.quoteLabel}>{side === 'bid' ? 'Best bid' : 'Best ask'}</div>
            {level === null ? (
              <div className={styles.quoteValue}>None displayed</div>
            ) : (
              <div>
                <span className={styles.quoteValue}>{level.price}</span>{' '}
                <span className={styles.quoteSize}>× {level.quantity}</span>
              </div>
            )}
          </div>
        );
      })}
      <p className={styles.quoteNote}>
        Sequence <span className="mono">{market.ingest_sequence}</span>, received{' '}
        <span className="mono">{formatMarketClock(market.receive_timestamp)}</span> UTC. Buys fill at the ask and
        sells at the bid, up to the displayed size.
      </p>
    </div>
  );
}
