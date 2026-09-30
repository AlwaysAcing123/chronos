// The one boundary between the application and the Chronos HTTP API. Every
// response is runtime-decoded before it reaches application state.
import type { ApiFailure, FailureCategory } from './errors';
import { ChronosApiError, failureCategory } from './errors';
import type { HttpResponse, TransportOptions } from './http';
import { decodeBody, failureOf, sendRequest } from './http';
import type { OrderRequest, SubmissionResponse } from './submission';
import { classifySubmission } from './submission';
import type {
  Account,
  ErrorBody,
  FillPage,
  Health,
  MarketData,
  Order,
  OrderEventPage,
  OrderPage,
  Portfolio,
  WorkingOrderPage,
} from './schemas';
import {
  AccountEnvelopeSchema,
  CancellationSchema,
  ErrorEnvelopeSchema,
  FillPageSchema,
  HealthSchema,
  MarketDataEnvelopeSchema,
  NotCancellableSchema,
  OrderEnvelopeSchema,
  OrderEventPageSchema,
  OrderPageSchema,
  PortfolioEnvelopeSchema,
  WorkingOrderPageSchema,
  isWorking,
} from './schemas';

// An account trades every instrument the server offers, so it is not
// created for one.
export interface CreateAccountRequest {
  readonly account_id: string;
  readonly starting_cash: string;
}

// The server's answer to "value this account now". Unusable market data and
// a failed valuation are answers, not transport failures.
export type PortfolioReading =
  | { readonly kind: 'valued'; readonly portfolio: Portfolio }
  | {
      readonly kind: 'unavailable';
      readonly category: 'market_data_unavailable' | 'valuation_failed';
      readonly error: ErrorBody;
    };

export type OrderLookup =
  | { readonly kind: 'recorded'; readonly order: Order }
  | { readonly kind: 'not_recorded' };

export type ReplayCommand = 'start' | 'pause' | 'resume';

// What a cancel request proves about the order.
export type CancelResponse =
  // Cancelled by this request, now or by an earlier try of the same ID.
  | {
      readonly kind: 'cancelled';
      readonly order: Order;
      readonly account: Account;
      readonly alreadyCancelled: boolean;
    }
  // Nothing was left to cancel; the order is shown as it is.
  | { readonly kind: 'not_cancellable'; readonly order: Order; readonly error: ErrorBody }
  // Definitely not cancelled by this request, and sending it again would not
  // change that.
  | { readonly kind: 'refused'; readonly error: ErrorBody; readonly category: FailureCategory }
  // The cancellation may or may not have been recorded.
  | { readonly kind: 'uncertain'; readonly failure: ApiFailure };

export interface ChronosClient {
  getHealth(signal?: AbortSignal): Promise<Health>;
  createAccount(request: CreateAccountRequest): Promise<Account>;
  getAccount(accountId: string, signal?: AbortSignal): Promise<Account>;
  getPortfolio(accountId: string, signal?: AbortSignal): Promise<PortfolioReading>;
  // `instrument` null lists every symbol; a cursor pages only the filter
  // that issued it.
  listOrders(
    accountId: string,
    instrument: string | null,
    cursor: string | null,
    signal?: AbortSignal,
  ): Promise<OrderPage>;
  listFills(
    accountId: string,
    instrument: string | null,
    cursor: string | null,
    signal?: AbortSignal,
  ): Promise<FillPage>;
  // Never cancellable: aborting would not stop the server from recording the
  // order, only hide the outcome.
  submitOrder(accountId: string, request: OrderRequest): Promise<SubmissionResponse>;
  lookupOrder(accountId: string, clientOrderId: string, signal?: AbortSignal): Promise<OrderLookup>;
  // Never cancellable, like a submission. Every retry reuses the ID.
  cancelOrder(accountId: string, clientOrderId: string, cancelRequestId: string): Promise<CancelResponse>;
  // In priority order, oldest first.
  listWorkingOrders(accountId: string, cursor: string | null, signal?: AbortSignal): Promise<WorkingOrderPage>;
  listOrderEvents(
    accountId: string,
    clientOrderId: string,
    cursor: string | null,
    signal?: AbortSignal,
  ): Promise<OrderEventPage>;
  getMarketData(signal?: AbortSignal): Promise<MarketData>;
  // Returns the market data after the command, or throws its typed error.
  controlReplay(command: ReplayCommand): Promise<MarketData>;
}

// Items per history page; "Load older" follows the server's keyset cursor.
export const HISTORY_PAGE_SIZE = 25;
// Working orders and one order's events page forward, oldest first.
export const FORWARD_PAGE_SIZE = 50;

export const DEFAULT_TRANSPORT: TransportOptions = {
  fetch: (input, init) => fetch(input, init),
  readTimeoutMs: 10_000,
  writeTimeoutMs: 20_000,
};

function accountPath(accountId: string): string {
  return `/api/v1/accounts/${encodeURIComponent(accountId)}`;
}

function historyPath(
  accountId: string,
  list: 'orders' | 'fills',
  instrument: string | null,
  cursor: string | null,
): string {
  const query = new URLSearchParams({ limit: String(HISTORY_PAGE_SIZE) });
  if (instrument !== null) {
    query.set('instrument', instrument);
  }
  if (cursor !== null) {
    query.set('cursor', cursor);
  }
  return `${accountPath(accountId)}/${list}?${query.toString()}`;
}

function forwardPath(path: string, cursor: string | null): string {
  const query = new URLSearchParams({ limit: String(FORWARD_PAGE_SIZE) });
  if (cursor !== null) {
    query.set('cursor', cursor);
  }
  return `${path}?${query.toString()}`;
}

// Categories that prove a cancel request changed nothing.
const CANCEL_REFUSED: ReadonlySet<FailureCategory> = new Set([
  'malformed_request',
  'account_not_found',
  'order_not_found',
  'duplicate_cancel_request_id',
]);

interface ExpectedCancellation {
  readonly accountId: string;
  readonly clientOrderId: string;
  readonly cancelRequestId: string;
}

function cancellationMatchesRequest(order: Order, expected: ExpectedCancellation): boolean {
  return (
    order.account_id === expected.accountId &&
    order.client_order_id === expected.clientOrderId &&
    order.cancellation_reason === 'user_requested' &&
    order.cancel_request_id === expected.cancelRequestId
  );
}

export function classifyCancellation(
  response: HttpResponse,
  expected: ExpectedCancellation,
): CancelResponse {
  if (response.status === 200) {
    const cancelled = CancellationSchema.safeParse(response.body);
    if (
      cancelled.success &&
      cancelled.data.account.account_id === expected.accountId &&
      cancellationMatchesRequest(cancelled.data.order, expected)
    ) {
      return {
        kind: 'cancelled',
        order: cancelled.data.order,
        account: cancelled.data.account,
        alreadyCancelled: cancelled.data.already_cancelled,
      };
    }
    return uncertainCancel(response.status, 'the cancellation does not match the API contract');
  }
  const envelope = ErrorEnvelopeSchema.safeParse(response.body);
  if (!envelope.success) {
    return uncertainCancel(response.status, 'the response matches neither a cancellation nor an error');
  }
  const failure: ApiFailure = { kind: 'api_error', status: response.status, error: envelope.data.error };
  const category = failureCategory(failure);
  if (category === 'order_not_cancellable') {
    const terminal = NotCancellableSchema.safeParse(response.body);
    return response.status === 409 &&
      terminal.success &&
      terminal.data.order.account_id === expected.accountId &&
      terminal.data.order.client_order_id === expected.clientOrderId &&
      !isWorking(terminal.data.order.status)
      ? { kind: 'not_cancellable', order: terminal.data.order, error: terminal.data.error }
      : uncertainCancel(response.status, 'the order that cannot be cancelled does not match the API contract');
  }
  const refusalStatusMatches =
    (category === 'malformed_request' && response.status === 400) ||
    ((category === 'account_not_found' || category === 'order_not_found') && response.status === 404) ||
    (category === 'duplicate_cancel_request_id' && response.status === 409);
  if (CANCEL_REFUSED.has(category) && refusalStatusMatches) {
    return { kind: 'refused', error: envelope.data.error, category };
  }
  // persistence_unavailable, internal_error, and anything unrecognised.
  return { kind: 'uncertain', failure };
}

function uncertainCancel(status: number, detail: string): CancelResponse {
  return { kind: 'uncertain', failure: { kind: 'unexpected_response', status, detail } };
}

function requireStatus(response: HttpResponse, status: number): HttpResponse {
  if (response.status !== status) {
    throw failureOf(response);
  }
  return response;
}

export function createChronosClient(transport: TransportOptions = DEFAULT_TRANSPORT): ChronosClient {
  const get = (path: string, signal?: AbortSignal) =>
    sendRequest(transport, { method: 'GET', path, signal });

  return {
    async getHealth(signal) {
      const response = await get('/health', signal);
      // 503 reports unavailable persistence with the same body.
      if (response.status !== 200 && response.status !== 503) {
        throw failureOf(response);
      }
      return decodeBody(HealthSchema, response);
    },

    async createAccount(request) {
      const response = await sendRequest(transport, {
        method: 'POST',
        path: '/api/v1/accounts',
        body: request,
      });
      return decodeBody(AccountEnvelopeSchema, requireStatus(response, 201)).account;
    },

    async getAccount(accountId, signal) {
      const response = await get(accountPath(accountId), signal);
      return decodeBody(AccountEnvelopeSchema, requireStatus(response, 200)).account;
    },

    async getPortfolio(accountId, signal) {
      const response = await get(`${accountPath(accountId)}/portfolio`, signal);
      if (response.status === 200) {
        return { kind: 'valued', portfolio: decodeBody(PortfolioEnvelopeSchema, response).portfolio };
      }
      const failure = failureOf(response);
      const category = failureCategory(failure.failure);
      if (
        failure.failure.kind === 'api_error' &&
        (category === 'market_data_unavailable' || category === 'valuation_failed')
      ) {
        return { kind: 'unavailable', category, error: failure.failure.error };
      }
      throw failure;
    },

    async listOrders(accountId, instrument, cursor, signal) {
      const response = await get(historyPath(accountId, 'orders', instrument, cursor), signal);
      return decodeBody(OrderPageSchema, requireStatus(response, 200));
    },

    async listFills(accountId, instrument, cursor, signal) {
      const response = await get(historyPath(accountId, 'fills', instrument, cursor), signal);
      return decodeBody(FillPageSchema, requireStatus(response, 200));
    },

    async submitOrder(accountId, request) {
      try {
        const response = await sendRequest(transport, {
          method: 'POST',
          path: `${accountPath(accountId)}/orders`,
          body: request,
        });
        return classifySubmission(response);
      } catch (error) {
        if (error instanceof ChronosApiError) {
          return { kind: 'uncertain', failure: error.failure };
        }
        throw error;
      }
    },

    async getMarketData(signal) {
      const response = await get('/api/v1/market-data', signal);
      return decodeBody(MarketDataEnvelopeSchema, requireStatus(response, 200)).market_data;
    },

    async controlReplay(command) {
      const response = await sendRequest(transport, {
        method: 'POST',
        path: `/api/v1/market-data/replay/${command}`,
        body: {},
      });
      return decodeBody(MarketDataEnvelopeSchema, requireStatus(response, 200)).market_data;
    },

    async cancelOrder(accountId, clientOrderId, cancelRequestId) {
      try {
        const response = await sendRequest(transport, {
          method: 'POST',
          path: `${accountPath(accountId)}/orders/${encodeURIComponent(clientOrderId)}/cancel`,
          body: { cancel_request_id: cancelRequestId },
        });
        return classifyCancellation(response, {
          accountId,
          clientOrderId,
          cancelRequestId,
        });
      } catch (error) {
        if (error instanceof ChronosApiError) {
          return { kind: 'uncertain', failure: error.failure };
        }
        throw error;
      }
    },

    async listWorkingOrders(accountId, cursor, signal) {
      const response = await get(forwardPath(`${accountPath(accountId)}/working-orders`, cursor), signal);
      return decodeBody(WorkingOrderPageSchema, requireStatus(response, 200));
    },

    async listOrderEvents(accountId, clientOrderId, cursor, signal) {
      const response = await get(
        forwardPath(`${accountPath(accountId)}/orders/${encodeURIComponent(clientOrderId)}/events`, cursor),
        signal,
      );
      return decodeBody(OrderEventPageSchema, requireStatus(response, 200));
    },

    async lookupOrder(accountId, clientOrderId, signal) {
      const response = await get(
        `${accountPath(accountId)}/orders/${encodeURIComponent(clientOrderId)}`,
        signal,
      );
      if (response.status === 200) {
        return { kind: 'recorded', order: decodeBody(OrderEnvelopeSchema, response).order };
      }
      const failure = failureOf(response);
      if (failureCategory(failure.failure) === 'order_not_found') {
        return { kind: 'not_recorded' };
      }
      throw failure;
    },
  };
}
