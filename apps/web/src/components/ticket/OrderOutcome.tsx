import { useEffect, useRef } from 'react';

import { failureCategory } from '../../api/errors';
import type { Order } from '../../api/schemas';
import { isWorking } from '../../api/schemas';
import {
  CANCELLATION_REASON,
  FAILURE,
  ORDER_STATUS,
  recordedOutcomeLabel,
  TIME_IN_FORCE,
} from '../../presentation/vocabulary';
import type { TicketState, UncertainCause } from '../../state/orderTicket';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { Facts } from '../ui/Facts';
import type { Fact } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { ReasonText } from '../ui/ReasonText';
import { RequestSummary } from './OrderReview';
import styles from './ticket.module.css';

type OutcomeState = Exclude<TicketState, { step: 'editing' } | { step: 'reviewing' }>;

interface OutcomeActions {
  readonly onSubmitAgain: () => void;
  readonly submitDisabled: boolean;
  readonly onCheckAgain: () => void;
  readonly onEdit: () => void;
  readonly onStartOver: () => void;
}

// A single fill has one price; several fills are described by quantity only.
function atPrice(order: Order): string {
  return order.fill !== null && order.fill_count === '1' ? ` at ${order.fill.price}` : '';
}

function recordedHeadline(order: Order): string {
  switch (order.status) {
    case 'working':
      return `Working: ${order.remaining_quantity} rests at limit ${order.limit_price ?? ''}`;
    case 'partially_filled':
      return `Partially filled ${order.filled_quantity} of ${order.quantity}${atPrice(order)}; ${order.remaining_quantity} still working`;
    case 'filled':
      return `Filled ${order.filled_quantity}${atPrice(order)}`;
    case 'partially_filled_and_cancelled':
      return `Partially filled ${order.filled_quantity} of ${order.quantity}${atPrice(order)}; ${order.remaining_quantity} cancelled`;
    case 'cancelled':
      return 'Cancelled: nothing was filled';
    case 'rejected':
    case 'faulted':
      return recordedOutcomeLabel(order.rejection?.code ?? order.status);
  }
}

function recordedFacts(order: Order): Fact[] {
  const facts: Fact[] = [
    { label: 'Client order ID', value: <span className="mono">{order.client_order_id}</span> },
    { label: 'Submitted (market time, UTC)', value: <MarketTime value={order.submitted_at} /> },
  ];
  if (order.fill !== null) {
    facts.push(
      { label: 'Fill price', value: order.fill.price },
      { label: 'Fill quantity', value: order.fill.quantity },
      { label: 'Notional', value: order.fill.notional },
      { label: 'Fee', value: order.fill.fee },
      { label: 'Fill ID', value: <span className="mono">{order.fill.fill_id}</span> },
    );
  }
  if (order.remaining_quantity !== '0') {
    facts.push({
      label: isWorking(order.status) ? 'Working quantity' : 'Unfilled quantity',
      value: order.remaining_quantity,
    });
  }
  if (order.time_in_force === 'gtc') {
    facts.push(
      { label: 'Time in force', value: TIME_IN_FORCE.gtc.label },
      { label: 'Filled so far', value: `${order.filled_quantity} in ${order.fill_count} fill(s)` },
      { label: 'Fee charged', value: order.fee_charged ? order.cumulative_fee : 'Not yet (paid with the first fill)' },
    );
  }
  if (isWorking(order.status)) {
    facts.push({
      label: 'Reserved while working',
      value: order.side === 'buy' ? `${order.reserved_cash} cash` : `${order.reserved_quantity} shares, ${order.reserved_cash} cash`,
    });
  }
  const { bid, ask, ingest_sequence } = order.market;
  facts.push({
    label: 'Quote used',
    value: (
      <span>
        {bid ? `${bid.price} × ${bid.quantity}` : 'no bid'} / {ask ? `${ask.price} × ${ask.quantity}` : 'no ask'}{' '}
        <span className="mono">(seq {ingest_sequence})</span>
      </span>
    ),
  });
  return facts;
}

function RecordedOutcome({ order, learnedFrom }: { readonly order: Order; readonly learnedFrom: 'response' | 'lookup' }) {
  const rejection = order.rejection;
  return (
    <>
      <Alert tone={ORDER_STATUS[order.status].tone} title={recordedHeadline(order)} live>
        <p>
          <Badge tone={ORDER_STATUS[order.status].tone}>{ORDER_STATUS[order.status].label}</Badge> Recorded in the
          paper journal.
        </p>
        {order.cancellation_reason !== null && <p>{CANCELLATION_REASON[order.cancellation_reason]}.</p>}
        {rejection !== null && (
          <p>
            <ReasonText error={rejection} />
            {rejection.field !== null && (
              <>
                {' '}
                (field <Code>{rejection.field}</Code>)
              </>
            )}
          </p>
        )}
        {rejection !== null && <p>A recorded order consumes its client order ID; a corrected order gets a new one.</p>}
        {isWorking(order.status) && (
          <p>
            The remainder rests as a working order: later quotes at or better than the limit fill it. Cancel it from
            Working orders.
          </p>
        )}
        {learnedFrom === 'lookup' && (
          <p>The first response was inconclusive, so this outcome was confirmed by looking up the client order ID.</p>
        )}
      </Alert>
      <Facts facts={recordedFacts(order)} />
    </>
  );
}

function causeText(cause: UncertainCause): string {
  return cause.kind === 'id_already_used'
    ? 'The server reported that this client order ID was already used.'
    : `The submission got no definitive response. ${FAILURE[failureCategory(cause.failure)].title}.`;
}

export function OrderOutcome({ state, actions }: { readonly state: OutcomeState; readonly actions: OutcomeActions }) {
  const container = useRef<HTMLDivElement>(null);
  // Keyboard focus follows the ticket from the submit button to its outcome.
  useEffect(() => {
    container.current?.focus();
  }, [state.step]);

  return (
    <div ref={container} className={styles.outcome} tabIndex={-1}>
      <OutcomeBody state={state} actions={actions} />
    </div>
  );
}

function OutcomeBody({ state, actions }: { readonly state: OutcomeState; readonly actions: OutcomeActions }) {
  switch (state.step) {
    case 'submitting':
      return (
        <>
          <Alert tone="info" title="Submitting paper order…" live />
          <RequestSummary request={state.request} />
        </>
      );
    case 'recorded':
      return (
        <>
          <RecordedOutcome order={state.order} learnedFrom={state.learnedFrom} />
          <div>
            <Button variant="primary" onClick={actions.onStartOver}>
              New order
            </Button>
          </div>
        </>
      );
    case 'not_recorded':
      return (
        <>
          <Alert
            tone="negative"
            title={`Not recorded: ${FAILURE[state.category].title}`}
            live
            actions={
              <>
                {state.transient && (
                  <Button variant="primary" onClick={actions.onSubmitAgain} disabled={actions.submitDisabled}>
                    Send again with the same ID
                  </Button>
                )}
                <Button onClick={actions.onEdit}>Edit order</Button>
              </>
            }
          >
            <p>{state.error.message}</p>
            {state.error.reason !== null && (
              <p>
                <ReasonText error={state.error} />
              </p>
            )}
            <p>Nothing was recorded, and the client order ID is still unused.</p>
          </Alert>
          <RequestSummary request={state.request} />
        </>
      );
    case 'reconciling':
      return (
        <>
          <Alert tone="caution" title="Outcome unknown: checking whether the order was recorded…" live>
            <p>{causeText(state.cause)}</p>
            <p>Looking up the client order ID before anything else is sent.</p>
          </Alert>
          <RequestSummary request={state.request} />
        </>
      );
    case 'unresolved':
      return (
        <>
          <Alert
            tone="caution"
            title="Outcome still unknown"
            live
            actions={
              <Button variant="primary" onClick={actions.onCheckAgain}>
                Check again
              </Button>
            }
          >
            <p>{causeText(state.cause)}</p>
            <p>
              The lookup also failed: {FAILURE[state.lookupFailure].title}. The order may or may not have been
              recorded. Do not place it again until its outcome is confirmed; the order history will show
              it if it was recorded.
            </p>
          </Alert>
          <RequestSummary request={state.request} />
        </>
      );
    case 'confirmed_absent':
      return (
        <>
          <Alert
            tone="caution"
            title="Confirmed not recorded"
            live
            actions={
              <>
                <Button variant="primary" onClick={actions.onSubmitAgain} disabled={actions.submitDisabled}>
                  Send again with the same ID
                </Button>
                <Button onClick={actions.onStartOver}>Discard order</Button>
              </>
            }
          >
            <p>{causeText(state.cause)}</p>
            <p>
              A lookup found no order with this client order ID. Sending it again with the same ID is safe: the
              server records each ID at most once per account. If you discard it instead, check the order history:
              a submission that was still being processed would appear there.
            </p>
          </Alert>
          <RequestSummary request={state.request} />
        </>
      );
    case 'id_conflict':
      return (
        <>
          <Alert
            tone="negative"
            title="This client order ID belongs to a different order"
            live
            actions={
              <Button variant="primary" onClick={actions.onStartOver}>
                New order
              </Button>
            }
          >
            <p>
              The account already recorded <Code>{state.existing.client_order_id}</Code> with different terms.
              Nothing new was recorded.
            </p>
          </Alert>
          <RequestSummary request={state.request} />
        </>
      );
  }
}
