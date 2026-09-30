import { useEffect, useRef } from 'react';

import type { OrderRequest } from '../../api/submission';
import { ORDER_TYPE, SIDE, SOURCE_KIND, TIME_IN_FORCE } from '../../presentation/vocabulary';
import { Button } from '../ui/Button';
import formStyles from '../form/form.module.css';
import styles from './ticket.module.css';

// Exactly what will be sent. Rendered by the review step and by outcomes
// that still refer to this request.
export function RequestSummary({ request }: { readonly request: OrderRequest }) {
  return (
    <dl className={styles.summary}>
      <dt>Side</dt>
      <dd className={styles.side}>{SIDE[request.side]}</dd>
      <dt>Symbol</dt>
      <dd>{request.instrument}</dd>
      <dt>Type</dt>
      <dd>{ORDER_TYPE[request.type]}</dd>
      <dt>Quantity</dt>
      <dd>{request.quantity}</dd>
      {request.limit_price !== undefined && (
        <>
          <dt>Limit price</dt>
          <dd>{request.limit_price}</dd>
        </>
      )}
      <dt>Time in force</dt>
      <dd>
        {TIME_IN_FORCE[request.time_in_force].label} ({TIME_IN_FORCE[request.time_in_force].short})
      </dd>
      <dt>Source</dt>
      <dd>
        {SOURCE_KIND[request.source.kind]} · <span className="mono">{request.source.id}</span>
      </dd>
      <dt>Client order ID</dt>
      <dd className="mono">{request.client_order_id}</dd>
    </dl>
  );
}

// What the time in force means for this order, in the terms the server
// applies. Amounts are the server's to compute: the review only names them.
export function reviewExplanation(request: OrderRequest): string {
  if (request.time_in_force === 'ioc') {
    return 'Simulated only. Immediate or cancel: the server executes it once against its current quote; any unfilled quantity is cancelled.';
  }
  const reserve =
    request.side === 'buy'
      ? 'cash for the limit price times the unfilled quantity, plus the fee until the first fill'
      : 'the unfilled shares, plus the fee from cash until the first fill';
  return (
    'Simulated only. Good til cancelled: the server fills what it can against its current quote, and the rest ' +
    'rests as a working order. Later quotes at or better than the limit fill it at the displayed price, up to the ' +
    `displayed size, until it fills or you cancel it. While it works it reserves ${reserve}. The fee is charged ` +
    'once, with the first fill.'
  );
}

export function OrderReview({
  request,
  onSubmit,
  onEdit,
  submitDisabled,
}: {
  readonly request: OrderRequest;
  readonly onSubmit: () => void;
  readonly onEdit: () => void;
  readonly submitDisabled: boolean;
}) {
  const submitButton = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    submitButton.current?.focus();
  }, []);
  return (
    <section className={styles.review} aria-labelledby="order-review-heading">
      <h3 id="order-review-heading" className={styles.reviewHeading}>
        Review paper order
      </h3>
      <RequestSummary request={request} />
      <p className={formStyles.hint}>{reviewExplanation(request)}</p>
      <div className={formStyles.actions}>
        <Button ref={submitButton} variant="primary" onClick={onSubmit} disabled={submitDisabled}>
          Submit paper order
        </Button>
        <Button onClick={onEdit}>Edit</Button>
      </div>
    </section>
  );
}
