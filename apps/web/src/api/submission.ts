// Classifies an order submission response by what it proves about the
// durable journal. The API contract: a body contains `order` exactly when
// the submission was recorded; persistence_unavailable and internal_error
// mean it may or may not have been.
import { sameDecimal } from './decimal';
import type { ApiFailure, FailureCategory } from './errors';
import { failureCategory } from './errors';
import type { HttpResponse } from './http';
import type { Account, ErrorBody, Order, OrderStatus, OrderType, Side } from './schemas';
import { ErrorEnvelopeSchema, RecordedSubmissionSchema } from './schemas';

// Exactly what the ticket sends. Decimals are the user's text, unchanged.
export interface OrderRequest {
  readonly client_order_id: string;
  readonly source: { readonly kind: 'manual'; readonly id: string };
  readonly instrument: string;
  readonly side: Side;
  readonly type: OrderType;
  readonly quantity: string;
  readonly limit_price?: string;
  readonly time_in_force: 'ioc';
}

export type SubmissionResponse =
  // Recorded in the journal, whether executed, rejected, or faulted.
  | { readonly kind: 'recorded'; readonly order: Order; readonly account: Account }
  // Definitely not recorded; the client order ID is still unused.
  | {
      readonly kind: 'not_recorded';
      readonly error: ErrorBody;
      readonly category: FailureCategory;
      // The cause may clear by itself, so resubmitting the same order is reasonable.
      readonly transient: boolean;
    }
  // The account already recorded this client order ID.
  | { readonly kind: 'id_already_used'; readonly error: ErrorBody }
  // The order may or may not have been recorded.
  | { readonly kind: 'uncertain'; readonly failure: ApiFailure };

// The status a recorded submission of each outcome returns.
const RECORDED_STATUS: Readonly<Record<OrderStatus, number>> = {
  filled: 201,
  partially_filled_and_cancelled: 201,
  cancelled: 201,
  rejected: 422,
  faulted: 500,
};

const TRANSIENT_CATEGORIES: ReadonlySet<FailureCategory> = new Set([
  'account_version_conflict',
  'market_data_unavailable',
]);

const DEFINITELY_UNRECORDED: ReadonlySet<FailureCategory> = new Set([
  'malformed_request',
  'account_not_found',
  'account_version_conflict',
  'market_data_unavailable',
]);

export function classifySubmission(response: HttpResponse): SubmissionResponse {
  const recorded = RecordedSubmissionSchema.safeParse(response.body);
  if (recorded.success) {
    const { order, account } = recorded.data;
    if (RECORDED_STATUS[order.status] === response.status) {
      return { kind: 'recorded', order, account };
    }
    return uncertain(response.status, 'the response status disagrees with the recorded order');
  }
  if (hasOrderKey(response.body)) {
    return uncertain(response.status, 'the recorded order does not match the API contract');
  }

  const envelope = ErrorEnvelopeSchema.safeParse(response.body);
  if (!envelope.success) {
    return uncertain(response.status, 'the response matches neither an order nor an error');
  }
  const error = envelope.data.error;
  const failure: ApiFailure = { kind: 'api_error', status: response.status, error };
  const category = failureCategory(failure);
  if (category === 'duplicate_client_order_id') {
    return { kind: 'id_already_used', error };
  }
  if (DEFINITELY_UNRECORDED.has(category)) {
    return { kind: 'not_recorded', error, category, transient: TRANSIENT_CATEGORIES.has(category) };
  }
  // persistence_unavailable, internal_error, and anything this client does
  // not recognise leave the outcome unknown.
  return { kind: 'uncertain', failure };
}

function hasOrderKey(body: unknown): boolean {
  return typeof body === 'object' && body !== null && 'order' in body;
}

function uncertain(status: number, detail: string): SubmissionResponse {
  return { kind: 'uncertain', failure: { kind: 'unexpected_response', status, detail } };
}

/**
 * Whether a recorded order is the one `request` describes. A lookup by
 * client order ID must not present someone else's order as this one.
 */
export function orderMatchesRequest(order: Order, request: OrderRequest): boolean {
  const limitPriceMatches =
    request.limit_price === undefined
      ? order.limit_price === null
      : order.limit_price !== null && sameDecimal(order.limit_price, request.limit_price);
  return (
    order.client_order_id === request.client_order_id &&
    order.source.kind === request.source.kind &&
    order.source.id === request.source.id &&
    order.instrument === request.instrument &&
    order.side === request.side &&
    order.type === request.type &&
    sameDecimal(order.quantity, request.quantity) &&
    limitPriceMatches
  );
}
