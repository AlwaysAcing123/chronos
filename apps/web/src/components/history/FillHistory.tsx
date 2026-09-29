import type { FillRecord } from '../../api/schemas';
import { useFillHistory } from '../../state/queries';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { SourceLabel } from '../ui/SourceLabel';
import type { Column } from './HistoryParts';
import { HistoryTable, HistoryView, SideText } from './HistoryParts';

const FILL_COLUMNS: readonly Column<FillRecord>[] = [
  { header: 'Filled (UTC)', cell: (fill) => <MarketTime value={fill.filled_at} stacked /> },
  { header: 'Fill ID', cell: (fill) => <span className="mono">{fill.fill_id}</span> },
  { header: 'Client order ID', cell: (fill) => <span className="mono">{fill.client_order_id}</span> },
  { header: 'Source', cell: (fill) => <SourceLabel source={fill.source} /> },
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
}: {
  readonly id?: string | undefined;
  readonly className?: string | undefined;
  readonly accountId: string;
}) {
  const history = useFillHistory(accountId);
  const fills = history.data?.pages.flatMap((page) => page.fills) ?? [];
  return (
    <Panel id={id} className={className} title="Fills" meta="Booked by accounting">
      <HistoryView
        query={history}
        itemCount={fills.length}
        noun="fills"
        emptyText="No fills booked yet. Only executed orders book fills; rejected and cancelled orders do not."
      >
        <HistoryTable
          label="Fill history"
          caption="Booked paper fills, newest first"
          columns={FILL_COLUMNS}
          rows={fills}
          rowKey={(fill) => fill.fill_id}
        />
      </HistoryView>
    </Panel>
  );
}
