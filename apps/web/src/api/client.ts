// The one boundary between the application and the Chronos HTTP API. Every
// response is runtime-decoded before it reaches application state.
import { ChronosApiError, failureCategory } from './errors';
import type { HttpResponse, TransportOptions } from './http';
import { decodeBody, failureOf, sendRequest } from './http';
import type { OrderRequest, SubmissionResponse } from './submission';
import { classifySubmission } from './submission';
import type { Account, ErrorBody, FillPage, Health, Order, OrderPage, Portfolio } from './schemas';
import {
  AccountEnvelopeSchema,
  FillPageSchema,
  HealthSchema,
  OrderEnvelopeSchema,
  OrderPageSchema,
  PortfolioEnvelopeSchema,
} from './schemas';

export interface CreateAccountRequest {
  readonly account_id: string;
  readonly instrument: string;
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

export interface ChronosClient {
  getHealth(signal?: AbortSignal): Promise<Health>;
  createAccount(request: CreateAccountRequest): Promise<Account>;
  getAccount(accountId: string, signal?: AbortSignal): Promise<Account>;
  getPortfolio(accountId: string, signal?: AbortSignal): Promise<PortfolioReading>;
  listOrders(accountId: string, cursor: string | null, signal?: AbortSignal): Promise<OrderPage>;
  listFills(accountId: string, cursor: string | null, signal?: AbortSignal): Promise<FillPage>;
  // Never cancellable: aborting would not stop the server from recording the
  // order, only hide the outcome.
  submitOrder(accountId: string, request: OrderRequest): Promise<SubmissionResponse>;
  lookupOrder(accountId: string, clientOrderId: string, signal?: AbortSignal): Promise<OrderLookup>;
}

// Items per history page; "Load older" follows the server's keyset cursor.
export const HISTORY_PAGE_SIZE = 25;

export const DEFAULT_TRANSPORT: TransportOptions = {
  fetch: (input, init) => fetch(input, init),
  readTimeoutMs: 10_000,
  writeTimeoutMs: 20_000,
};

function accountPath(accountId: string): string {
  return `/api/v1/accounts/${encodeURIComponent(accountId)}`;
}

function historyPath(accountId: string, list: 'orders' | 'fills', cursor: string | null): string {
  const query = new URLSearchParams({ limit: String(HISTORY_PAGE_SIZE) });
  if (cursor !== null) {
    query.set('cursor', cursor);
  }
  return `${accountPath(accountId)}/${list}?${query.toString()}`;
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

    async listOrders(accountId, cursor, signal) {
      const response = await get(historyPath(accountId, 'orders', cursor), signal);
      return decodeBody(OrderPageSchema, requireStatus(response, 200));
    },

    async listFills(accountId, cursor, signal) {
      const response = await get(historyPath(accountId, 'fills', cursor), signal);
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
