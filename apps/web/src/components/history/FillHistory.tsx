import { useState } from 'react';

import type { FillRecord } from '../../api/schemas';
import { useFillHistory } from '../../state/queries';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { SourceLabel } from '../ui/SourceLabel';
import type { Column } from './HistoryParts';
import { HistoryTable, HistoryView, SideText, SymbolFilter } from './HistoryParts';
import styles from './history.module.css';

const FILL_COLUMNS: readonly Column<FillRecord>[] = [
  { header: 'Filled (UTC)', cell: (fill) => <MarketTime value={fill.filled_at} stacked /> },
  { header: 'Fill ID', cell: (fill) => <span className="mono">{fill.fill_id}</span> },
  { header: 'Client order ID', cell: (fill) => <span className="mono">{fill.client_order_id}</span> },
  { header: 'Source', cell: (fill) => <SourceLabel source={fill.source} /> },
  { header: 'Symbol', cell: (fill) => fill.instrument },
  { header: 'Side', cell: (fill) => <SideText side={fill.side} /> },
  { header: 'Price', numeric: true, cell: (fill) => fill.price },
  { header: 'Quantity', numeric: true, cell: (fill) => fill.quantity },
  { header: 'Notional', numeric: true, cell: (fill) => fill.notional },
  { header: 'Fee', numeric: true, cell: (fill) => fill.fee },
];

export function FillHistory({
  id,
  className,
  accountId,
  symbols,
}: {
  readonly id?: string | undefined;
  readonly className?: string | undefined;
  readonly accountId: string;
  // The symbols to offer as filters.
  readonly symbols: readonly string[];
}) {
  const [symbol, setSymbol] = useState<string | null>(null);
  const history = useFillHistory(accountId, symbol);
  const fills = history.data?.pages.flatMap((page) => page.fills) ?? [];
  return (
    <Panel id={id} className={className} title="Fills" meta="Booked by accounting">
      <div className={styles.stack}>
        <SymbolFilter symbols={symbols} value={symbol} onChange={setSymbol} />
        <HistoryView
          query={history}
          itemCount={fills.length}
          noun="fills"
          emptyText={
            symbol === null
              ? 'No fills booked yet. Only executed orders book fills; rejected and cancelled orders do not.'
              : `No ${symbol} fills booked for this account.`
          }
        >
          <HistoryTable
            label="Fill history"
            caption={`Booked paper fills${symbol === null ? '' : ` for ${symbol}`}, newest first`}
            columns={FILL_COLUMNS}
            rows={fills}
            rowKey={(fill) => fill.fill_id}
          />
        </HistoryView>
      </div>
    </Panel>
  );
}
