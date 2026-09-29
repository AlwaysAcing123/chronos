import type { UseQueryResult } from '@tanstack/react-query';

import type { PortfolioReading } from '../../api/client';
import type { MarketObservation, MarketStream } from '../../api/schemas';
import { QUALITY_FLAG } from '../../presentation/vocabulary';
import { useMarketData } from '../../state/queries';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Facts } from '../ui/Facts';
import { FailureNotice } from '../ui/FailureNotice';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ReasonText } from '../ui/ReasonText';
import { Unavailable } from '../ui/DecimalText';
import styles from './workspace.module.css';

function StreamFacts({ market }: { readonly market: MarketStream }) {
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
  return <QuoteFacts market={reading.portfolio.market} />;
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
  const marketData = useMarketData();
  const stream = marketData.status === 'success' ? marketData.data.stream : null;
  return (
    <Panel id={id} className={className} title={`Market · ${instrument}`} meta="Server-owned market state">
      <div className={styles.group}>
        <h3 className={styles.groupTitle}>Quote</h3>
        <QuoteSection portfolio={portfolio} />
      </div>
      <div className={styles.group}>
        <h3 className={styles.groupTitle}>Stream</h3>
        {stream !== null ? (
          <StreamFacts market={stream} />
        ) : marketData.status === 'error' ? (
          <FailureNotice
            error={marketData.error}
            onRetry={() => void marketData.refetch()}
            retrying={marketData.isFetching}
          />
        ) : (
          <span className={styles.note}>Loading stream status…</span>
        )}
        {stream?.interruption != null && (
          <Alert tone="negative" title="The market-data stream is interrupted">
            {stream.interruption} Orders and valuation stay unavailable until the server restarts.
          </Alert>
        )}
      </div>
    </Panel>
  );
}
