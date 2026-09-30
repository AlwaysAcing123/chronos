import { FRACTION_DIGITS, validatePositiveDecimal } from '../../api/decimal';
import type { OrderType, Side, TimeInForce } from '../../api/schemas';
import type { OrderRequest } from '../../api/submission';
import { decimalProblemMessage } from '../form/messages';

// What the user has typed so far. Decimals stay text from input to request.
export interface OrderDraft {
  readonly side: Side;
  readonly type: OrderType;
  readonly quantity: string;
  readonly limitPrice: string;
  // Chosen for limit orders; a market order is always immediate-or-cancel.
  readonly timeInForce: TimeInForce;
}

export const INITIAL_DRAFT: OrderDraft = {
  side: 'buy',
  type: 'market',
  quantity: '',
  limitPrice: '',
  timeInForce: 'ioc',
};

// Without authentication there is no principal, so a manual order names the
// channel it came from. An authenticated user will replace this later.
export const MANUAL_SOURCE_ID = 'web-ticket';

export interface DraftErrors {
  readonly quantity?: string;
  readonly limitPrice?: string;
}

/** Usability checks only; the server validates again and is authoritative. */
export function validateOrderDraft(draft: OrderDraft): DraftErrors {
  const quantity = validatePositiveDecimal(draft.quantity, 'quantity');
  const limitPrice = draft.type === 'limit' ? validatePositiveDecimal(draft.limitPrice, 'price') : null;
  return {
    ...(quantity.ok ? {} : { quantity: decimalProblemMessage(quantity.problem, FRACTION_DIGITS.quantity) }),
    ...(limitPrice === null || limitPrice.ok
      ? {}
      : { limitPrice: decimalProblemMessage(limitPrice.problem, FRACTION_DIGITS.price) }),
  };
}

export function hasDraftErrors(errors: DraftErrors): boolean {
  return errors.quantity !== undefined || errors.limitPrice !== undefined;
}

/**
 * The exact request for a validated draft. A market order carries no limit
 * price and is always immediate-or-cancel, whatever was chosen for a limit.
 */
export function buildOrderRequest(draft: OrderDraft, instrument: string, clientOrderId: string): OrderRequest {
  const common = {
    client_order_id: clientOrderId,
    source: { kind: 'manual', id: MANUAL_SOURCE_ID },
    instrument,
    side: draft.side,
    quantity: draft.quantity,
  } as const;
  return draft.type === 'limit'
    ? { ...common, type: 'limit', limit_price: draft.limitPrice, time_in_force: draft.timeInForce }
    : { ...common, type: 'market', time_in_force: 'ioc' };
}
