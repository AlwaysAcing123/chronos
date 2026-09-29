import { useEffect, useRef } from 'react';

import type { OrderRequest } from '../../api/submission';
import { ORDER_TYPE, SIDE, SOURCE_KIND } from '../../presentation/vocabulary';
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
      <dd>Immediate or cancel</dd>
      <dt>Source</dt>
      <dd>
        {SOURCE_KIND[request.source.kind]} · <span className="mono">{request.source.id}</span>
      </dd>
      <dt>Client order ID</dt>
      <dd className="mono">{request.client_order_id}</dd>
    </dl>
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
      <p className={formStyles.hint}>
        Simulated only. The server executes it once against its current quote; any unfilled quantity is cancelled.
      </p>
      <div className={formStyles.actions}>
        <Button ref={submitButton} variant="primary" onClick={onSubmit} disabled={submitDisabled}>
          Submit paper order
        </Button>
        <Button onClick={onEdit}>Edit</Button>
      </div>
    </section>
  );
}
