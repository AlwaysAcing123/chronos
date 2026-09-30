import { useState } from 'react';

import type { Order } from '../../api/schemas';
import { FAILURE, ORDER_STATUS } from '../../presentation/vocabulary';
import type { CancelState } from '../../state/orderCancellation';
import { canRetryCancel, isSettled, useOrderCancellation } from '../../state/orderCancellation';
import { useWorkingOrders } from '../../state/queries';
import { failureCategory } from '../../api/errors';
import type { Column } from '../history/HistoryParts';
import { HistoryTable, HistoryView, SideText } from '../history/HistoryParts';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { SourceLabel } from '../ui/SourceLabel';
import { OrderLifecycle } from './OrderLifecycle';
import styles from './orders.module.css';

// What the order holds back, exactly as the server reports it.
function Reserved({ order }: { readonly order: Order }) {
  return (
    <span className={styles.reserved}>
      <span>{order.reserved_cash}</span>
      <span className={styles.reservedDetail}>
        {order.side === 'sell' ? `cash, and ${order.reserved_quantity} shares` : 'cash'}
        {order.fee_charged ? '' : ' · incl. fee'}
      </span>
    </span>
  );
}

function notice(order: string, state: CancelState): { tone: 'info' | 'caution' | 'negative' | 'positive' | 'neutral'; title: string; body: string } {
  switch (state.step) {
    case 'cancelling':
      return { tone: 'info', title: `Cancelling ${order}…`, body: `Cancel request ${state.requestId}.` };
    case 'reconciling':
      return {
        tone: 'caution',
        title: `Cancel of ${order}: outcome unknown, checking the order…`,
        body: `${FAILURE[failureCategory(state.failure)].title}. Reading the order before anything else is sent.`,
      };
    case 'not_cancelled':
      return {
        tone: 'caution',
        title: `${order} is still working`,
        body:
          'The cancel request got no definitive response and the order has not been cancelled. Sending the same ' +
          `request (${state.requestId}) again is safe: the server cancels at most once for it.`,
      };
    case 'unresolved':
      return {
        tone: 'caution',
        title: `Cancel of ${order}: outcome still unknown`,
        body:
          `The lookup also failed: ${FAILURE[state.lookupFailure].title}. The remainder may or may not have been ` +
          `cancelled. Check again, or send the same request (${state.requestId}) again; it cannot cancel twice.`,
      };
    case 'cancelled':
      return {
        tone: 'positive',
        title: `${order}: remainder cancelled`,
        body:
          (state.learnedFrom === 'lookup'
            ? 'Confirmed by reading the order after an inconclusive response. '
            : state.learnedFrom === 'retry'
              ? 'An earlier try of this request had already cancelled it. '
              : '') +
          `${state.order.filled_quantity} of ${state.order.quantity} filled; ${state.order.remaining_quantity} ` +
          'cancelled and released.',
      };
    case 'not_cancellable':
      return {
        tone: 'neutral',
        title: `${order}: nothing left to cancel`,
        body: `It is now ${ORDER_STATUS[state.order.status].label.toLowerCase()}; ${state.order.filled_quantity} of ${state.order.quantity} filled.`,
      };
    case 'refused':
      return { tone: 'negative', title: `Cancel of ${order} refused`, body: `${FAILURE[state.category].title}. ${state.error.message}` };
  }
}

export function WorkingOrders({
  id,
  className,
  accountId,
}: {
  readonly id?: string | undefined;
  readonly className?: string | undefined;
  readonly accountId: string;
}) {
  const working = useWorkingOrders(accountId);
  const cancellation = useOrderCancellation(accountId);
  const [shown, setShown] = useState<string | null>(null);
  const orders = working.data?.pages.flatMap((page) => page.orders) ?? [];

  const actionFor = (order: Order) => {
    const state = cancellation.states.get(order.client_order_id);
    const pending = state !== undefined && !isSettled(state) && !canRetryCancel(state);
    return (
      <span className={styles.reserved}>
        <Button
          onClick={() => {
            cancellation.cancel(order.client_order_id);
          }}
          disabled={pending}
          aria-label={`${state !== undefined && canRetryCancel(state) ? 'Retry cancel of' : 'Cancel remainder of'} ${order.client_order_id}`}
        >
          {pending ? 'Cancelling…' : state !== undefined && canRetryCancel(state) ? 'Retry cancel' : 'Cancel remainder'}
        </Button>
        <Button
          variant="quiet"
          onClick={() => {
            setShown(shown === order.client_order_id ? null : order.client_order_id);
          }}
          aria-expanded={shown === order.client_order_id}
          aria-label={`Lifecycle of ${order.client_order_id}`}
        >
          Lifecycle
        </Button>
      </span>
    );
  };

  const columns: readonly Column<Order>[] = [
    { header: 'Submitted (UTC)', cell: (order) => <MarketTime value={order.submitted_at} stacked /> },
    { header: 'Client order ID', cell: (order) => <span className="mono">{order.client_order_id}</span> },
    { header: 'Symbol', cell: (order) => order.instrument },
    { header: 'Side', cell: (order) => <SideText side={order.side} /> },
    { header: 'Limit', numeric: true, cell: (order) => order.limit_price ?? '—' },
    { header: 'Quantity', numeric: true, cell: (order) => order.quantity },
    { header: 'Filled', numeric: true, cell: (order) => order.filled_quantity },
    { header: 'Remaining', numeric: true, cell: (order) => order.remaining_quantity },
    {
      header: 'Status',
      cell: (order) => <Badge tone={ORDER_STATUS[order.status].tone}>{ORDER_STATUS[order.status].label}</Badge>,
    },
    { header: 'Source', cell: (order) => <SourceLabel source={order.source} /> },
    { header: 'Reserved', numeric: true, cell: (order) => <Reserved order={order} /> },
    { header: 'Action', cell: actionFor },
  ];

  const notices = [...cancellation.states.entries()];
  return (
    <Panel id={id} className={className} title="Working orders" meta="Good til cancelled · priority order">
      <div className={styles.stack}>
        <HistoryView
          query={working}
          itemCount={orders.length}
          noun="working orders"
          emptyText="No working orders. A good-til-cancelled limit order rests here until it fills or you cancel it."
          ordering="priority"
        >
          <HistoryTable
            label="Working order list"
            caption="Working paper orders in priority order, with what each reserves"
            columns={columns}
            rows={orders}
            rowKey={(order) => order.client_order_id}
          />
        </HistoryView>
        {notices.length > 0 && (
          <ul className={styles.notices} aria-label="Cancellations">
            {notices.map(([order, state]) => {
              const text = notice(order, state);
              return (
                <li key={order}>
                  <Alert
                    tone={text.tone}
                    title={text.title}
                    live
                    actions={
                      <>
                        {state.step === 'unresolved' && (
                          <Button
                            onClick={() => {
                              cancellation.checkAgain(order);
                            }}
                          >
                            Check again
                          </Button>
                        )}
                        {canRetryCancel(state) && (
                          <Button
                            variant="primary"
                            onClick={() => {
                              cancellation.cancel(order);
                            }}
                          >
                            Send the same cancel again
                          </Button>
                        )}
                        {isSettled(state) && (
                          <Button
                            variant="quiet"
                            onClick={() => {
                              cancellation.dismiss(order);
                            }}
                          >
                            Dismiss
                          </Button>
                        )}
                      </>
                    }
                  >
                    <p>{text.body}</p>
                  </Alert>
                </li>
              );
            })}
          </ul>
        )}
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
