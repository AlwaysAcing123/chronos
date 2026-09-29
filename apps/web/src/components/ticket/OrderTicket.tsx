import { useState } from 'react';

import { newClientOrderId } from '../../api/identifiers';
import type { Account, InstrumentAvailability } from '../../api/schemas';
import { useOrderTicket } from '../../state/orderTicket';
import { SelectField } from '../form/Field';
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
  symbols,
  selected,
  onSelect,
  quotes,
}: {
  readonly id?: string | undefined;
  readonly account: Account;
  // The symbols this server trades, in configuration order.
  readonly symbols: readonly string[];
  // The symbol chosen in the watchlist or here; null until symbols load.
  readonly selected: string | null;
  readonly onSelect: (symbol: string) => void;
  // Each symbol's current quote, for reference; null while loading.
  readonly quotes: readonly InstrumentAvailability[] | null;
}) {
  const ticket = useOrderTicket(account.account_id);
  const [draft, setDraft] = useState<OrderDraft>(INITIAL_DRAFT);
  const [errors, setErrors] = useState<DraftErrors>({});
  const halted = account.status === 'halted';
  const state = ticket.state;
  // Once reviewed, the ticket shows the order's own symbol, whatever is
  // selected elsewhere afterwards.
  const symbol = state.step === 'editing' ? selected : state.request.instrument;
  const availability = symbol === null ? undefined : quotes?.find((item) => item.instrument === symbol);
  // The server remains authoritative, but do not offer a submission while
  // its latest market resource explicitly says this symbol is unusable.
  const quoteKnownUnavailable = availability?.execution_quote_available === false;

  function review() {
    const found = validateOrderDraft(draft);
    setErrors(found);
    if (!hasDraftErrors(found) && selected !== null && !quoteKnownUnavailable) {
      // One client order ID per reviewed order; every retry reuses it.
      ticket.review(buildOrderRequest(draft, selected, newClientOrderId()));
    }
  }

  return (
    <Panel id={id} title="Order ticket" meta="Paper · immediate or cancel">
      <div className={styles.ticket}>
        {state.step === 'editing' && selected !== null && (
          <SelectField
            label="Symbol"
            value={selected}
            options={symbols.map((option) => ({ value: option, label: option }))}
            onChange={onSelect}
            hint="Also selects the symbol in the watchlist."
          />
        )}
        {symbol !== null && (
          <QuoteReference
            instrument={symbol}
            availability={availability}
            loading={quotes === null}
          />
        )}
        {halted && (
          <Alert tone="negative" title="This account is halted">
            A processing fault halted it; the server records every new order on it as faulted.
          </Alert>
        )}
        {state.step === 'editing' &&
          (selected === null ? (
            <p className={styles.quoteNote}>Loading the symbols this server trades…</p>
          ) : (
            <OrderForm
              account={account}
              instrument={selected}
              draft={draft}
              errors={errors}
              disabled={halted}
              reviewDisabled={quoteKnownUnavailable}
              onChange={(next) => {
                setDraft(next);
                setErrors({});
              }}
              onReview={review}
            />
          ))}
        {state.step === 'reviewing' && (
          <OrderReview
            request={state.request}
            onSubmit={ticket.submit}
            onEdit={ticket.edit}
            submitDisabled={quoteKnownUnavailable}
          />
        )}
        {state.step !== 'editing' && state.step !== 'reviewing' && (
          <OrderOutcome
            state={state}
            actions={{
              onSubmitAgain: ticket.submit,
              submitDisabled: quoteKnownUnavailable,
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
