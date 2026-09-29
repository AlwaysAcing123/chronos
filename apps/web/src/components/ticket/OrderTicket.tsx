import { useState } from 'react';

import type { PortfolioReading } from '../../api/client';
import { newClientOrderId } from '../../api/identifiers';
import type { Account } from '../../api/schemas';
import { useOrderTicket } from '../../state/orderTicket';
import { Alert } from '../ui/Alert';
import { Panel } from '../ui/Panel';
import { OrderForm } from './OrderForm';
import { OrderOutcome } from './OrderOutcome';
import { OrderReview } from './OrderReview';
import { QuoteReference } from './QuoteReference';
import type { DraftErrors, OrderDraft } from './orderDraft';
import { buildOrderRequest, hasDraftErrors, INITIAL_DRAFT, validateOrderDraft } from './orderDraft';
import styles from './ticket.module.css';

export function OrderTicket({
  id,
  account,
  reading,
}: {
  readonly id?: string | undefined;
  readonly account: Account;
  // The latest portfolio reading, for the reference quote; null while loading.
  readonly reading: PortfolioReading | null;
}) {
  const ticket = useOrderTicket(account.account_id);
  const [draft, setDraft] = useState<OrderDraft>(INITIAL_DRAFT);
  const [errors, setErrors] = useState<DraftErrors>({});
  const halted = account.status === 'halted';

  function review() {
    const found = validateOrderDraft(draft);
    setErrors(found);
    if (!hasDraftErrors(found)) {
      // One client order ID per reviewed order; every retry reuses it.
      ticket.review(buildOrderRequest(draft, account.instrument, newClientOrderId()));
    }
  }

  const state = ticket.state;
  return (
    <Panel id={id} title="Order ticket" meta="Paper · immediate or cancel">
      <div className={styles.ticket}>
        <QuoteReference reading={reading} />
        {halted && (
          <Alert tone="negative" title="This account is halted">
            A processing fault halted it; the server records every new order on it as faulted.
          </Alert>
        )}
        {state.step === 'editing' && (
          <OrderForm
            account={account}
            draft={draft}
            errors={errors}
            disabled={halted}
            onChange={(next) => {
              setDraft(next);
              setErrors({});
            }}
            onReview={review}
          />
        )}
        {state.step === 'reviewing' && (
          <OrderReview request={state.request} onSubmit={ticket.submit} onEdit={ticket.edit} />
        )}
        {state.step !== 'editing' && state.step !== 'reviewing' && (
          <OrderOutcome
            state={state}
            actions={{
              onSubmitAgain: ticket.submit,
              onCheckAgain: ticket.checkAgain,
              onEdit: ticket.edit,
              onStartOver: ticket.startOver,
            }}
          />
        )}
      </div>
    </Panel>
  );
}
