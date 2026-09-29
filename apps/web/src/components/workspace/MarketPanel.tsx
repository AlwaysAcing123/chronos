import type { UseQueryResult } from '@tanstack/react-query';

import type { PortfolioReading } from '../../api/client';
import type { Health, MarketObservation } from '../../api/schemas';
import { QUALITY_FLAG } from '../../presentation/vocabulary';
import { useHealth } from '../../state/queries';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Facts } from '../ui/Facts';
import { FailureNotice } from '../ui/FailureNotice';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ReasonText } from '../ui/ReasonText';
import { Unavailable } from '../ui/DecimalText';
import styles from './workspace.module.css';

function StreamFacts({ health }: { readonly health: Health }) {
  const market = health.market_data;
  return (
    <Facts
      facts={[
        { label: 'Stream', value: <span className="mono">{market.stream_id}</span> },
        {
          label: 'State',
          value:
            market.state === 'interrupted' ? (
              <Badge tone="negative">Interrupted</Badge>
            ) : (
              <Badge tone="info">Accepting</Badge>
            ),
        },
        {
          label: 'Last ingest sequence',
          value: market.last_ingest_sequence ?? <Unavailable>None yet</Unavailable>,
        },
        {
          label: 'Market time (UTC)',
          value:
            market.market_time === null ? (
              <Unavailable>No events</Unavailable>
            ) : (
              <MarketTime value={market.market_time} />
            ),
        },
      ]}
    />
  );
}

function QuoteFacts({ market }: { readonly market: MarketObservation }) {
  const level = (side: 'bid' | 'ask') => {
    const book = market[side];
    return book === null ? (
      <Unavailable>None displayed</Unavailable>
    ) : (
      <span className={styles.bookSide}>
        {book.price} <span className="mono">× {book.quantity}</span>
      </span>
    );
  };
  return (
    <Facts
      facts={[
        { label: 'Best bid × size', value: level('bid') },
        { label: 'Best ask × size', value: level('ask') },
        { label: 'Observation sequence', value: market.ingest_sequence },
        { label: 'Venue', value: market.venue ?? <Unavailable>Not reported</Unavailable> },
        { label: 'Event time (UTC)', value: <MarketTime value={market.event_timestamp} /> },
        { label: 'Receive time (UTC)', value: <MarketTime value={market.receive_timestamp} /> },
        {
          label: 'Quality flags',
          value:
            market.quality_flags.length === 0 ? (
              'None'
            ) : (
              <span className={styles.flags}>
                {market.quality_flags.map((flag) => (
                  <Badge key={flag} tone={QUALITY_FLAG[flag].tone}>
                    {QUALITY_FLAG[flag].label}
                  </Badge>
                ))}
              </span>
            ),
        },
      ]}
    />
  );
}

function QuoteSection({ portfolio }: { readonly portfolio: UseQueryResult<PortfolioReading, unknown> }) {
  if (portfolio.status === 'pending') {
    return <span className={styles.note}>Loading quote…</span>;
  }
  if (portfolio.status === 'error') {
    return (
      <FailureNotice
        error={portfolio.error}
        onRetry={() => void portfolio.refetch()}
        retrying={portfolio.isFetching}
      />
    );
  }
  const reading = portfolio.data;
  if (reading.kind === 'unavailable') {
    return (
      <Alert tone="caution" title="No usable quote for this account">
        <ReasonText error={reading.error} />
      </Alert>
    );
  }
  const market = reading.portfolio.market;
  return (
    <>
      <QuoteFacts market={market} />
      {market.quality_flags.includes('replay') && (
        <p className={styles.note}>
          Replayed data: the server replays its recording at startup and serves the final state, so this quote does
          not move while the server runs.
        </p>
      )}
    </>
  );
}

export function MarketPanel({
  id,
  className,
  instrument,
  portfolio,
}: {
  readonly id?: string | undefined;
  readonly className?: string | undefined;
  readonly instrument: string;
  readonly portfolio: UseQueryResult<PortfolioReading, unknown>;
}) {
  const health = useHealth();
  return (
    <Panel id={id} className={className} title={`Market · ${instrument}`} meta="Server-owned market state">
      <div className={styles.group}>
        <h3 className={styles.groupTitle}>Quote</h3>
        <QuoteSection portfolio={portfolio} />
      </div>
      <div className={styles.group}>
        <h3 className={styles.groupTitle}>Stream</h3>
        {health.status === 'success' ? (
          <StreamFacts health={health.data} />
        ) : health.status === 'error' ? (
          <FailureNotice error={health.error} onRetry={() => void health.refetch()} retrying={health.isFetching} />
        ) : (
          <span className={styles.note}>Loading stream status…</span>
        )}
        {health.status === 'success' && health.data.market_data.interruption !== null && (
          <Alert tone="negative" title="The market-data stream is interrupted">
            {health.data.market_data.interruption} Orders and valuation stay unavailable until the server restarts.
          </Alert>
        )}
      </div>
    </Panel>
  );
}
