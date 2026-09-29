import type { InstrumentAvailability } from '../../api/schemas';
import { formatMarketClock } from '../../presentation/format';
import { Alert } from '../ui/Alert';
import { ReasonText } from '../ui/ReasonText';
import styles from './ticket.module.css';

// The symbol's current quote, for reference only: the server executes against
// its own quote at the moment of submission.
export function QuoteReference({
  instrument,
  availability,
  loading,
}: {
  readonly instrument: string;
  readonly availability: InstrumentAvailability | undefined;
  readonly loading: boolean;
}) {
  if (loading) {
    return <p className={styles.quoteNote}>Loading the {instrument} quote…</p>;
  }
  const market = availability?.quote ?? null;
  if (market === null) {
    return (
      <Alert tone="caution" title={`No usable ${instrument} quote right now`}>
        {availability?.unavailable_reason == null ? (
          'The server reports no quote for this symbol'
        ) : (
          <ReasonText error={{ message: '', reason: availability.unavailable_reason }} />
        )}
        . The server checks again when the order is submitted.
      </Alert>
    );
  }
  return (
    <div className={styles.quote} role="group" aria-label={`Reference quote for ${instrument}`}>
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
        {instrument} sequence <span className="mono">{market.ingest_sequence}</span>, received{' '}
        <span className="mono">{formatMarketClock(market.receive_timestamp)}</span> UTC. Buys fill at the ask and
        sells at the bid, up to the displayed size.
      </p>
    </div>
  );
}
