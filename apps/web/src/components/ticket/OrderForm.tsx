import type { SubmitEvent } from 'react';

import type { Account } from '../../api/schemas';
import { nanosecondsAsSeconds } from '../../presentation/format';
import { Choice, TextField } from '../form/Field';
import formStyles from '../form/form.module.css';
import { Button } from '../ui/Button';
import type { DraftErrors, OrderDraft } from './orderDraft';
import styles from './ticket.module.css';

export function OrderForm({
  account,
  instrument,
  draft,
  errors,
  disabled,
  reviewDisabled,
  onChange,
  onReview,
}: {
  readonly account: Account;
  readonly instrument: string;
  readonly draft: OrderDraft;
  readonly errors: DraftErrors;
  readonly disabled: boolean;
  readonly reviewDisabled: boolean;
  readonly onChange: (draft: OrderDraft) => void;
  readonly onReview: () => void;
}) {
  const limits = account.risk_limits;
  const held = account.positions.find((position) => position.instrument === instrument);
  function submit(event: SubmitEvent<HTMLFormElement>) {
    event.preventDefault();
    onReview();
  }
  return (
    <form className={formStyles.form} onSubmit={submit} noValidate aria-label="Paper order">
      <Choice
        legend="Side"
        name="side"
        value={draft.side}
        options={[
          { value: 'buy', label: 'Buy' },
          { value: 'sell', label: 'Sell' },
        ]}
        onChange={(side) => {
          onChange({ ...draft, side });
        }}
        disabled={disabled}
      />
      <Choice
        legend="Order type"
        name="type"
        value={draft.type}
        options={[
          { value: 'market', label: 'Market' },
          { value: 'limit', label: 'Limit' },
        ]}
        onChange={(type) => {
          onChange({ ...draft, type });
        }}
        disabled={disabled}
      />
      <TextField
        label={`Quantity (${instrument})`}
        name="quantity"
        value={draft.quantity}
        onChange={(event) => {
          onChange({ ...draft, quantity: event.target.value });
        }}
        inputMode="decimal"
        autoComplete="off"
        spellCheck={false}
        error={errors.quantity}
        hint={`Up to 8 decimal places. Maximum ${limits.maximum_order_quantity} per order.`}
        disabled={disabled}
      />
      {draft.type === 'limit' && (
        <TextField
          label="Limit price"
          name="limit_price"
          value={draft.limitPrice}
          onChange={(event) => {
            onChange({ ...draft, limitPrice: event.target.value });
          }}
          inputMode="decimal"
          autoComplete="off"
          spellCheck={false}
          error={errors.limitPrice}
          hint={draft.side === 'buy' ? 'Fills only if the ask is at or below this price.' : 'Fills only if the bid is at or above this price.'}
          disabled={disabled}
        />
      )}
      {draft.type === 'limit' ? (
        <Choice
          legend="Time in force"
          name="time_in_force"
          value={draft.timeInForce}
          options={[
            { value: 'ioc', label: 'Immediate or cancel (IOC)' },
            { value: 'gtc', label: 'Good til cancelled (GTC)' },
          ]}
          onChange={(timeInForce) => {
            onChange({ ...draft, timeInForce });
          }}
          disabled={disabled}
          hint={
            draft.timeInForce === 'gtc'
              ? 'Whatever does not fill now rests as a working order until later quotes fill it or you cancel it.'
              : 'Executes once against the current quote; whatever does not fill is cancelled.'
          }
        />
      ) : (
        <div className={styles.fixedTerm}>
          <span>Time in force</span>
          <span>Immediate or cancel (IOC). A market order cannot rest.</span>
        </div>
      )}
      {/* Risk limits are fixed when the account is created, so they can never be stale. The position limit
          applies to each symbol separately; cash is shared by all of them. */}
      <div className={styles.context} role="group" aria-label="Position and account limits">
        <div className={styles.contextItem}>
          <span className={styles.contextLabel}>Held ({instrument})</span>
          <span className={styles.contextValue}>{held?.quantity ?? 'None'}</span>
        </div>
        {held !== undefined && held.reserved_quantity !== '0' && (
          <div className={styles.contextItem}>
            <span className={styles.contextLabel}>Available to sell ({instrument})</span>
            <span className={styles.contextValue}>{held.available_quantity}</span>
          </div>
        )}
        <div className={styles.contextItem}>
          <span className={styles.contextLabel}>Available cash</span>
          <span className={styles.contextValue}>{account.available_cash}</span>
        </div>
        {account.reserved_cash !== '0' && (
          <div className={styles.contextItem}>
            <span className={styles.contextLabel}>Reserved by working orders</span>
            <span className={styles.contextValue}>{account.reserved_cash}</span>
          </div>
        )}
        <div className={styles.contextItem}>
          <span className={styles.contextLabel}>Fee per fill</span>
          <span className={styles.contextValue}>{limits.fee_per_fill}</span>
        </div>
        <div className={styles.contextItem}>
          <span className={styles.contextLabel}>Max notional</span>
          <span className={styles.contextValue}>{limits.maximum_order_notional}</span>
        </div>
        <div className={styles.contextItem}>
          <span className={styles.contextLabel}>Max position per symbol</span>
          <span className={styles.contextValue}>{limits.maximum_position_quantity}</span>
        </div>
        <div className={styles.contextItem}>
          <span className={styles.contextLabel}>Max quote age</span>
          <span className={styles.contextValue}>{nanosecondsAsSeconds(limits.maximum_quote_age_ns)} s</span>
        </div>
      </div>
      <div className={formStyles.actions}>
        <Button type="submit" variant="primary" disabled={disabled || reviewDisabled}>
          Review order
        </Button>
      </div>
    </form>
  );
}
