import { useState } from 'react';

import type { Order } from '../../api/schemas';
import { CANCELLATION_REASON, ORDER_STATUS, ORDER_TYPE, TIME_IN_FORCE } from '../../presentation/vocabulary';
import { useOrderHistory } from '../../state/queries';
import { OrderLifecycle } from '../orders/OrderLifecycle';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ReasonText } from '../ui/ReasonText';
import { SourceLabel } from '../ui/SourceLabel';
import type { Column } from './HistoryParts';
import { HistoryTable, HistoryView, SideText, SymbolFilter } from './HistoryParts';
import styles from './history.module.css';

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
  { header: 'Symbol', cell: (order) => order.instrument },
  { header: 'Side', cell: (order) => <SideText side={order.side} /> },
  { header: 'Type', cell: (order) => ORDER_TYPE[order.type] },
  { header: 'Quantity', numeric: true, cell: (order) => order.quantity },
  { header: 'Limit', numeric: true, cell: (order) => order.limit_price ?? '—' },
  { header: 'TIF', cell: (order) => TIME_IN_FORCE[order.time_in_force].short },
  {
    header: 'Status',
    cell: (order) => <Badge tone={ORDER_STATUS[order.status].tone}>{ORDER_STATUS[order.status].label}</Badge>,
  },
  { header: 'Filled', numeric: true, cell: (order) => order.filled_quantity },
  { header: 'Remaining', numeric: true, cell: (order) => order.remaining_quantity },
  // One price only when there is one fill; several fills have several.
  {
    header: 'Fill price',
    numeric: true,
    cell: (order) => (order.fill !== null && order.fill_count === '1' ? order.fill.price : order.fill_count === '0' ? '—' : `${order.fill_count} fills`),
  },
  { header: 'Detail', wraps: true, cell: (order) => <OrderDetail order={order} /> },
];

export function OrderBlotter({
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
  const [shown, setShown] = useState<string | null>(null);
  const history = useOrderHistory(accountId, symbol);
  const orders = history.data?.pages.flatMap((page) => page.orders) ?? [];
  const columns: readonly Column<Order>[] = [
    ...ORDER_COLUMNS,
    {
      header: 'Lifecycle',
      cell: (order) => (
        <Button
          variant="quiet"
          onClick={() => {
            setShown(shown === order.client_order_id ? null : order.client_order_id);
          }}
          aria-expanded={shown === order.client_order_id}
          aria-label={`Lifecycle of ${order.client_order_id}`}
        >
          Events
        </Button>
      ),
    },
  ];
  return (
    <Panel id={id} className={className} title="Orders" meta="Durable journal · manual and model share one path">
      <div className={styles.stack}>
        <SymbolFilter symbols={symbols} value={symbol} onChange={setSymbol} />
        <HistoryView
          query={history}
          itemCount={orders.length}
          noun="orders"
          emptyText={
            symbol === null ? 'No orders recorded for this account yet.' : `No ${symbol} orders recorded for this account.`
          }
        >
          <HistoryTable
            label="Order history"
            caption={`Recorded paper orders${symbol === null ? '' : ` for ${symbol}`}, newest first`}
            columns={columns}
            rows={orders}
            rowKey={(order) => order.client_order_id}
          />
        </HistoryView>
        {shown !== null && (
          <OrderLifecycle
            accountId={accountId}
            clientOrderId={shown}
            onClose={() => {
              setShown(null);
            }}
          />
        )}
      </div>
    </Panel>
  );
}
