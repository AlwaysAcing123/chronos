import type { ErrorBody } from './schemas';

// Why a request produced no usable resource. Recorded order outcomes, even
// rejections, are resources rather than failures (see submission.ts).
export type ApiFailure =
  // The server answered with its documented error envelope.
  | { readonly kind: 'api_error'; readonly status: number; readonly error: ErrorBody }
  // Something answered, but not with the documented contract.
  | { readonly kind: 'unexpected_response'; readonly status: number; readonly detail: string }
  // No response: the request failed in transit or timed out.
  | { readonly kind: 'network'; readonly detail: string };

export class ChronosApiError extends Error {
  override readonly name = 'ChronosApiError';
  readonly failure: ApiFailure;

  constructor(failure: ApiFailure) {
    super(failureSummary(failure));
    this.failure = failure;
  }
}

// The failure categories the interface distinguishes. Codes that only a
// client bug could produce (route_not_found, method_not_allowed,
// payload_too_large, unsupported_media_type) and codes this client does not
// know are unexpected responses.
export type FailureCategory =
  | 'malformed_request'
  | 'account_not_found'
  | 'order_not_found'
  | 'account_already_exists'
  | 'duplicate_client_order_id'
  | 'account_version_conflict'
  | 'unsupported_instrument'
  | 'invalid_starting_cash'
  | 'market_data_unavailable'
  | 'valuation_failed'
  | 'persistence_unavailable'
  | 'internal_error'
  | 'invalid_replay_transition'
  | 'replay_control_unavailable'
  | 'unexpected_response'
  | 'network';

const CATEGORY_BY_CODE: Readonly<Record<string, FailureCategory>> = {
  malformed_request: 'malformed_request',
  account_not_found: 'account_not_found',
  order_not_found: 'order_not_found',
  account_already_exists: 'account_already_exists',
  duplicate_client_order_id: 'duplicate_client_order_id',
  account_version_conflict: 'account_version_conflict',
  unsupported_instrument: 'unsupported_instrument',
  invalid_starting_cash: 'invalid_starting_cash',
  market_data_unavailable: 'market_data_unavailable',
  valuation_failed: 'valuation_failed',
  persistence_unavailable: 'persistence_unavailable',
  internal_error: 'internal_error',
  invalid_replay_transition: 'invalid_replay_transition',
  replay_control_unavailable: 'replay_control_unavailable',
};

export function failureCategory(failure: ApiFailure): FailureCategory {
  switch (failure.kind) {
    case 'api_error':
      return CATEGORY_BY_CODE[failure.error.code] ?? 'unexpected_response';
    case 'unexpected_response':
      return 'unexpected_response';
    case 'network':
      return 'network';
  }
}

export function categoryOf(error: unknown): FailureCategory {
  return error instanceof ChronosApiError ? failureCategory(error.failure) : 'unexpected_response';
}

export function isAccountNotFound(error: unknown): boolean {
  return categoryOf(error) === 'account_not_found';
}

function failureSummary(failure: ApiFailure): string {
  switch (failure.kind) {
    case 'api_error':
      return `${String(failure.status)} ${failure.error.code}: ${failure.error.message}`;
    case 'unexpected_response':
      return `unexpected response (${String(failure.status)}): ${failure.detail}`;
    case 'network':
      return failure.detail;
  }
}
