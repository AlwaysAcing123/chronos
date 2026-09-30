import type { OrderEvent } from '../../api/schemas';
import { ORDER_EVENT } from '../../presentation/vocabulary';
import { useOrderEvents } from '../../state/queries';
import { HistoryView } from '../history/HistoryParts';
import { Button } from '../ui/Button';
import { MarketTime } from '../ui/MarketTime';
import styles from './orders.module.css';

function EventItem({ event }: { readonly event: OrderEvent }) {
  return (
    <li className={styles.event}>
      <span className={styles.eventSequence}>{event.sequence}.</span>
      <span className={styles.eventBody}>
        <span>{ORDER_EVENT[event.type]}</span>
        <span>
          <MarketTime value={event.occurred_at} />
        </span>
        {event.fill !== null && (
          <span className={styles.eventFill}>
            {event.fill.quantity} at {event.fill.price} · notional {event.fill.notional} · fee {event.fill.fee} ·{' '}
            <span className="mono">{event.fill.fill_id}</span>
          </span>
        )}
      </span>
    </li>
  );
}

// Everything that happened to one order, oldest first, from the durable
// event log: its submission, acceptance, each fill, and how it ended.
export function OrderLifecycle({
  accountId,
  clientOrderId,
  onClose,
}: {
  readonly accountId: string;
  readonly clientOrderId: string;
  readonly onClose: () => void;
}) {
  const events = useOrderEvents(accountId, clientOrderId);
  const items = events.data?.pages.flatMap((page) => page.events) ?? [];
  const headingId = `lifecycle-${clientOrderId}`;
  return (
    <section className={styles.lifecycle} aria-labelledby={headingId}>
      <div className={styles.lifecycleHeading}>
        <h3 id={headingId}>
          Lifecycle of <span className="mono">{clientOrderId}</span>
        </h3>
        <Button variant="quiet" onClick={onClose}>
          Close lifecycle
        </Button>
      </div>
      <HistoryView
        query={events}
        itemCount={items.length}
        noun="events"
        emptyText="No events recorded for this order."
        ordering="oldest"
      >
        <ol className={styles.events} aria-label={`Events of ${clientOrderId}`}>
          {items.map((event) => (
            <EventItem key={event.sequence} event={event} />
          ))}
        </ol>
      </HistoryView>
    </section>
  );
}
