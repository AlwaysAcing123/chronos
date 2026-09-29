import { useId } from 'react';

import type { InstrumentAvailability, MarketObservation, MarketStream, OpenPosition } from '../../api/schemas';
import { QUALITY_FLAG, reasonLabel } from '../../presentation/vocabulary';
import { useMarketData } from '../../state/queries';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Facts } from '../ui/Facts';
import { FailureNotice } from '../ui/FailureNotice';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ReasonText } from '../ui/ReasonText';
import { Unavailable } from '../ui/DecimalText';
import type { Column } from '../history/HistoryParts';
import { HistoryTable } from '../history/HistoryParts';
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

function BookLevelText({ market, side }: { readonly market: MarketObservation | null; readonly side: 'bid' | 'ask' }) {
  const level = market?.[side] ?? null;
  return level === null ? (
    <Unavailable>—</Unavailable>
  ) : (
    <span>
      {level.price} <span className="mono">× {level.quantity}</span>
    </span>
  );
}

// One row per configured symbol. The radio in each row selects the symbol for
// the quote below and the order ticket; the selected row is also highlighted.
function Watchlist({
  instruments,
  positions,
  selected,
  onSelect,
}: {
  readonly instruments: readonly InstrumentAvailability[];
  readonly positions: readonly OpenPosition[];
  readonly selected: string | null;
  readonly onSelect: (instrument: string) => void;
}) {
  const name = useId();
  const held = (instrument: string) => positions.find((position) => position.instrument === instrument);
  const columns: readonly Column<InstrumentAvailability>[] = [
    {
      header: 'Symbol',
      cell: (item) => (
        <span className={styles.watchSymbol}>
          <label className={styles.watchSymbol}>
            <input
              type="radio"
              name={name}
              value={item.instrument}
              checked={item.instrument === selected}
              onChange={() => {
                onSelect(item.instrument);
              }}
            />
            <span>{item.instrument}</span>
          </label>
          {/* Visual only: the checked radio already announces the selection. */}
          {item.instrument === selected && (
            <span className={styles.selectedMark} aria-hidden="true">
              Selected
            </span>
          )}
        </span>
      ),
    },
    { header: 'Bid × size', numeric: true, cell: (item) => <BookLevelText market={item.quote} side="bid" /> },
    { header: 'Ask × size', numeric: true, cell: (item) => <BookLevelText market={item.quote} side="ask" /> },
    {
      header: 'Quote',
      cell: (item) =>
        item.unavailable_reason === null ? (
          <Badge tone="info">Available</Badge>
        ) : (
          <Badge tone="caution">{reasonLabel(item.unavailable_reason)}</Badge>
        ),
    },
    {
      header: 'Position',
      numeric: true,
      cell: (item) => held(item.instrument)?.quantity ?? <Unavailable>None</Unavailable>,
    },
  ];
  return (
    <fieldset className={styles.watchlist}>
      <legend className="visually-hidden">Choose the symbol to quote and trade</legend>
      <HistoryTable
        label="Watchlist"
        caption="Configured symbols, their current quotes, and positions held"
        columns={columns}
        rows={instruments}
        rowKey={(item) => item.instrument}
        rowClassName={(item) => (item.instrument === selected ? styles.selectedRow : undefined)}
      />
    </fieldset>
  );
}

function SelectedQuote({ item }: { readonly item: InstrumentAvailability | undefined }) {
  if (item === undefined) {
    return <span className={styles.note}>Choose a symbol to see its quote.</span>;
  }
  if (item.quote === null) {
    return (
      <Alert tone="caution" title={`No usable quote for ${item.instrument}`}>
        <ReasonText error={{ message: '', reason: item.unavailable_reason }} />. Orders for {item.instrument} are not
        recorded until one arrives.
      </Alert>
    );
  }
  return <QuoteFacts market={item.quote} />;
}

export function MarketPanel({
  id,
  className,
  selected,
  onSelect,
  positions,
}: {
  readonly id?: string | undefined;
  readonly className?: string | undefined;
  readonly selected: string | null;
  readonly onSelect: (instrument: string) => void;
  // Open positions, to show what is held beside each quote.
  readonly positions: readonly OpenPosition[];
}) {
  const marketData = useMarketData();
  const data = marketData.status === 'success' ? marketData.data : null;
  const stream = data?.stream ?? null;
  const selectedItem = data?.instruments.find((item) => item.instrument === selected);
  return (
    <Panel id={id} className={className} title="Market" meta="Server-owned market state">
      <div className={styles.group}>
        <h3 className={styles.groupTitle}>Watchlist</h3>
        {data !== null ? (
          <Watchlist instruments={data.instruments} positions={positions} selected={selected} onSelect={onSelect} />
        ) : marketData.status === 'error' ? (
          <FailureNotice
            error={marketData.error}
            onRetry={() => void marketData.refetch()}
            retrying={marketData.isFetching}
          />
        ) : (
          <span className={styles.note}>Loading quotes…</span>
        )}
      </div>
      {data !== null && (
        <div className={styles.group}>
          <h3 className={styles.groupTitle}>Quote{selected === null ? '' : ` · ${selected}`}</h3>
          <SelectedQuote item={selectedItem} />
        </div>
      )}
      <div className={styles.group}>
        <h3 className={styles.groupTitle}>Stream</h3>
        {stream !== null ? (
          <StreamFacts market={stream} />
        ) : marketData.status === 'error' ? (
          <span className={styles.note}>Stream status is unavailable.</span>
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
