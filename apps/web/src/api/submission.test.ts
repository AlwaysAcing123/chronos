import { describe, expect, it } from 'vitest';

import {
  accountJson,
  errorJson,
  faultedOrderJson,
  filledOrderJson,
  partialOrderJson,
  PERSISTENCE_UNAVAILABLE_MESSAGE,
  rejectedOrderJson,
  riskRejectionJson,
} from '../test/fixtures';
import { OrderSchema } from './schemas';
import type { OrderRequest } from './submission';
import { classifySubmission, orderMatchesRequest } from './submission';

describe('classifySubmission', () => {
  it('treats every response carrying an order as recorded', () => {
    expect(classifySubmission({ status: 201, body: { order: partialOrderJson(), account: accountJson() } }).kind)
      .toBe('recorded');
    expect(
      classifySubmission({
        status: 422,
        body: { error: riskRejectionJson(), order: rejectedOrderJson(), account: accountJson() },
      }).kind,
    ).toBe('recorded');
    const fault = faultedOrderJson();
    expect(
      classifySubmission({ status: 500, body: { error: fault.rejection, order: fault, account: accountJson() } })
        .kind,
    ).toBe('recorded');
  });

  it('cannot trust a recorded order whose status disagrees with the HTTP status', () => {
    const result = classifySubmission({
      status: 201,
      body: { error: riskRejectionJson(), order: rejectedOrderJson(), account: accountJson() },
    });
    expect(result.kind).toBe('uncertain');
  });

  it('treats a malformed recorded order as uncertain, never as not recorded', () => {
    const result = classifySubmission({
      status: 201,
      body: { order: partialOrderJson({ quantity: 10 }), account: accountJson() },
    });
    expect(result.kind).toBe('uncertain');
  });

  it('knows which failures leave the outcome unknown', () => {
    expect(
      classifySubmission({ status: 503, body: errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE) }),
    ).toMatchObject({ kind: 'uncertain', failure: { kind: 'api_error', status: 503 } });
    expect(classifySubmission({ status: 500, body: errorJson('internal_error', 'internal server error') }).kind)
      .toBe('uncertain');
    expect(classifySubmission({ status: 502, body: { detail: 'bad gateway' } }).kind).toBe('uncertain');
    expect(classifySubmission({ status: 400, body: errorJson('brand_new_code', 'unknown') }).kind).toBe('uncertain');
  });

  it('knows which failures prove nothing was recorded', () => {
    expect(
      classifySubmission({
        status: 503,
        body: errorJson('market_data_unavailable', 'no top of book', { reason: 'no_top_of_book' }),
      }),
    ).toMatchObject({ kind: 'not_recorded', category: 'market_data_unavailable', transient: true });
    expect(
      classifySubmission({ status: 409, body: errorJson('account_version_conflict', 'the account changed') }),
    ).toMatchObject({ kind: 'not_recorded', transient: true });
    expect(
      classifySubmission({
        status: 400,
        body: errorJson('malformed_request', 'quantity has too many fractional digits', {
          field: 'quantity',
          reason: 'too_many_fractional_digits',
        }),
      }),
    ).toMatchObject({ kind: 'not_recorded', category: 'malformed_request', transient: false });
  });

  it('reports a reused client order ID separately', () => {
    expect(
      classifySubmission({ status: 409, body: errorJson('duplicate_client_order_id', 'already used') }).kind,
    ).toBe('id_already_used');
  });
});

describe('orderMatchesRequest', () => {
  const request: OrderRequest = {
    client_order_id: 'sell-1',
    source: { kind: 'manual', id: 'web-ticket' },
    instrument: 'AAPL',
    side: 'sell',
    type: 'limit',
    quantity: '1.50',
    limit_price: '300.0',
    time_in_force: 'ioc',
  };
  const recorded = OrderSchema.parse(filledOrderJson());

  it('recognises the recorded order by value, not spelling', () => {
    expect(orderMatchesRequest(recorded, request)).toBe(true);
  });

  it('rejects an order with the same ID but different terms', () => {
    expect(orderMatchesRequest(recorded, { ...request, quantity: '2' })).toBe(false);
    expect(orderMatchesRequest(recorded, { ...request, side: 'buy' })).toBe(false);
    const market: OrderRequest = {
      client_order_id: request.client_order_id,
      source: request.source,
      instrument: request.instrument,
      side: request.side,
      type: 'market',
      quantity: request.quantity,
      time_in_force: 'ioc',
    };
    expect(orderMatchesRequest(recorded, market)).toBe(false);
  });
});
