import type { Order } from '../../api/schemas';
import { CANCELLATION_REASON, ORDER_STATUS, ORDER_TYPE } from '../../presentation/vocabulary';
import { useOrderHistory } from '../../state/queries';
import { Badge } from '../ui/Badge';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ReasonText } from '../ui/ReasonText';
import { SourceLabel } from '../ui/SourceLabel';
import type { Column } from './HistoryParts';
import { HistoryTable, HistoryView, SideText } from './HistoryParts';

function OrderDetail({ order }: { readonly order: Order }) {
  if (order.rejection !== null) {
    return <ReasonText error={order.rejection} />;
  }
  return order.cancellation_reason === null ? <>—</> : <>{CANCELLATION_REASON[order.cancellation_reason]}</>;
}

const ORDER_COLUMNS: readonly Column<Order>[] = [
  { header: 'Submitted (UTC)', cell: (order) => <MarketTime value={order.submitted_at} stacked /> },
  { header: 'Client order ID', cell: (order) => <span className="mono">{order.client_order_id}</span> },
  { header: 'Source', cell: (order) => <SourceLabel source={order.source} /> },
  { header: 'Instrument', cell: (order) => order.instrument },
  { header: 'Side', cell: (order) => <SideText side={order.side} /> },
  { header: 'Type', cell: (order) => ORDER_TYPE[order.type] },
  { header: 'Quantity', numeric: true, cell: (order) => order.quantity },
  { header: 'Limit', numeric: true, cell: (order) => order.limit_price ?? '—' },
  {
    header: 'Status',
    cell: (order) => <Badge tone={ORDER_STATUS[order.status].tone}>{ORDER_STATUS[order.status].label}</Badge>,
  },
  { header: 'Filled', numeric: true, cell: (order) => order.filled_quantity },
  { header: 'Fill price', numeric: true, cell: (order) => order.fill?.price ?? '—' },
  { header: 'Detail', wraps: true, cell: (order) => <OrderDetail order={order} /> },
];

export function OrderBlotter({
  id,
  className,
  accountId,
}: {
  readonly id?: string | undefined;
  readonly className?: string | undefined;
  readonly accountId: string;
}) {
  const history = useOrderHistory(accountId);
  const orders = history.data?.pages.flatMap((page) => page.orders) ?? [];
  return (
    <Panel id={id} className={className} title="Orders" meta="Durable journal · manual and model share one path">
      <HistoryView query={history} itemCount={orders.length} noun="orders" emptyText="No orders recorded for this account yet.">
        <HistoryTable
          label="Order history"
          caption="Recorded paper orders, newest first"
          columns={ORDER_COLUMNS}
          rows={orders}
          rowKey={(order) => order.client_order_id}
        />
      </HistoryView>
    </Panel>
  );
}
