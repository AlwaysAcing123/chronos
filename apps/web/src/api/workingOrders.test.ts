// The good-til-cancelled contract as the browser decodes it: working states,
// reservations, cancellations, lifecycles, and every invariant the views
// rely on. A record that breaks one is refused, never shown.
import { describe, expect, it } from 'vitest';

import { createFakeApi } from '../test/fakeApi';
import {
  ACCOUNT_ID,
  accountJson,
  errorJson,
  healthJson,
  orderEventJson,
  partialOrderJson,
  partiallyFilledWorkingOrderJson,
  PERSISTENCE_UNAVAILABLE_MESSAGE,
  userCancelledOrderJson,
  workingOrderJson,
} from '../test/fixtures';
import { FORWARD_PAGE_SIZE } from './client';
import {
  AccountSchema,
  HealthSchema,
  OrderEventPageSchema,
  OrderSchema,
  WorkingOrderPageSchema,
} from './schemas';

const accountPath = `/api/v1/accounts/${ACCOUNT_ID}`;

describe('good-til-cancelled orders', () => {
  it('decode working, partially filled, and client-cancelled orders exactly', () => {
    const working = OrderSchema.parse(workingOrderJson());
    expect(working.status).toBe('working');
    expect(working.time_in_force).toBe('gtc');
    expect(working.reserved_cash).toBe('3360.35');
    expect(working.priority).toBe('41');
    expect(working.fee_charged).toBe(false);
    const partial = OrderSchema.parse(partiallyFilledWorkingOrderJson());
    expect(partial.remaining_quantity).toBe('6');
    expect(partial.filled_notional).toBe('1344');
    expect(partial.cumulative_fee).toBe('0.35');
    const cancelled = OrderSchema.parse(userCancelledOrderJson());
    expect(cancelled.cancellation_reason).toBe('user_requested');
    expect(cancelled.cancel_request_id).toBe('cancel-1');
    expect(cancelled.reserved_cash).toBe('0');
  });

  it('refuse orders whose state, quantities, fee, or reservation do not fit', () => {
    const refused = (overrides: Record<string, unknown>, base = workingOrderJson()) =>
      OrderSchema.safeParse({ ...base, ...overrides }).success;
    // Only a good-til-cancelled order works, and a market order never does.
    expect(refused({ time_in_force: 'ioc' })).toBe(false);
    expect(refused({ type: 'market', limit_price: null })).toBe(false);
    // Status and quantities agree.
    expect(refused({ filled_quantity: '1' })).toBe(false);
    expect(refused({ status: 'filled' })).toBe(false);
    expect(refused({ remaining_quantity: '9' })).toBe(false);
    // The first fill charges the fee.
    expect(refused({ fee_charged: true })).toBe(false);
    expect(refused({ fill_count: '1' })).toBe(false);
    // A terminal order reserves nothing; a working one has a priority.
    expect(refused({ reserved_cash: '1' }, userCancelledOrderJson())).toBe(false);
    expect(refused({ priority: null })).toBe(false);
    expect(refused({ priority: '1' }, partialOrderJson())).toBe(false);
    // A client cancellation names its request, and only ends a GTC order.
    expect(refused({ cancel_request_id: null }, userCancelledOrderJson())).toBe(false);
    expect(refused({ cancellation_reason: 'displayed_liquidity_exhausted', cancel_request_id: null }, userCancelledOrderJson())).toBe(false);
    expect(refused({ cancellation_reason: 'user_requested', cancel_request_id: 'c' }, partialOrderJson())).toBe(false);
    // Unknown statuses and time-in-force values fail closed.
    expect(refused({ status: 'expired' })).toBe(false);
    expect(refused({ time_in_force: 'day' })).toBe(false);
    // An amount is never a JSON number.
    expect(refused({ reserved_cash: 3360.35 })).toBe(false);
  });

  it('keep immediate-or-cancel orders decoding as before', () => {
    const order = OrderSchema.parse(partialOrderJson());
    expect(order.status).toBe('partially_filled_and_cancelled');
    expect(order.priority).toBeNull();
    expect(order.remaining_quantity).toBe(order.unfilled_quantity);
  });

  it('require the server-computed reservation fields of an account', () => {
    const account = AccountSchema.parse(accountJson({ reserved_cash: '3360.35', available_cash: '95794.68', working_order_count: '1' }));
    expect(account.available_cash).toBe('95794.68');
    expect(AccountSchema.safeParse({ ...accountJson(), reserved_cash: undefined }).success).toBe(false);
    expect(AccountSchema.safeParse({ ...accountJson(), available_cash: undefined }).success).toBe(false);
  });

  it('accept only working orders on the working-orders page', () => {
    expect(WorkingOrderPageSchema.safeParse({ orders: [workingOrderJson()], next_cursor: null }).success).toBe(true);
    expect(WorkingOrderPageSchema.safeParse({ orders: [partialOrderJson()], next_cursor: null }).success).toBe(false);
  });

  it('decode lifecycles whose fill events carry their fills', () => {
    const fill = partiallyFilledWorkingOrderJson().fill ?? {
      fill_id: 'gtc-1:fill:0',
      side: 'buy',
      price: '336',
      quantity: '4',
      notional: '1344',
      fee: '0.35',
      filled_at: '2026-09-25T13:40:00.000000000Z',
    };
    const page = OrderEventPageSchema.parse({
      events: [
        orderEventJson(),
        orderEventJson({ sequence: '2', type: 'order_accepted' }),
        orderEventJson({ sequence: '4', type: 'fill_applied', fill }),
      ],
      next_cursor: null,
    });
    expect(page.events.map((event) => event.type)).toEqual(['order_submitted', 'order_accepted', 'fill_applied']);
    expect(OrderEventPageSchema.safeParse({ events: [orderEventJson({ type: 'fill_applied' })], next_cursor: null }).success).toBe(false);
    expect(OrderEventPageSchema.safeParse({ events: [orderEventJson({ type: 'order_expired' })], next_cursor: null }).success).toBe(false);
  });

  it('decode the matcher in health when the server reports it', () => {
    const matching = {
      state: 'retrying',
      observations: '9',
      account_matches: '4',
      fills: '2',
      retries: '1',
      failed_account_matches: '0',
      total_matching_ns: '1500',
      longest_matching_ns: '700',
    };
    expect(HealthSchema.parse(healthJson({ order_matching: matching })).order_matching?.state).toBe('retrying');
    expect(HealthSchema.parse(healthJson()).order_matching).toBeUndefined();
    expect(HealthSchema.safeParse(healthJson({ order_matching: { ...matching, state: 'paused' } })).success).toBe(false);
  });
});

describe('cancel requests', () => {
  const cancelPath = `${accountPath}/orders/gtc-1/cancel`;

  it('send the cancel request ID and decode the cancelled order', async () => {
    const api = createFakeApi();
    api.on('POST', cancelPath, {
      status: 200,
      body: { order: userCancelledOrderJson(), account: accountJson(), already_cancelled: false },
    });
    const response = await api.client.cancelOrder(ACCOUNT_ID, 'gtc-1', 'cancel-1');
    expect(api.requestsTo('POST', cancelPath)[0]?.body).toEqual({ cancel_request_id: 'cancel-1' });
    expect(response.kind).toBe('cancelled');
    if (response.kind === 'cancelled') {
      expect(response.order.status).toBe('partially_filled_and_cancelled');
      expect(response.alreadyCancelled).toBe(false);
    }
  });

  it('treats a cancellation response for anything else as uncertain', async () => {
    const api = createFakeApi();
    const outcome = async (body: unknown) => {
      api.on('POST', cancelPath, { status: 200, body });
      return (await api.client.cancelOrder(ACCOUNT_ID, 'gtc-1', 'cancel-1')).kind;
    };
    expect(
      await outcome({
        order: userCancelledOrderJson({ account_id: 'another-account' }),
        account: accountJson(),
        already_cancelled: false,
      }),
    ).toBe('uncertain');
    expect(
      await outcome({
        order: userCancelledOrderJson({ client_order_id: 'another-order' }),
        account: accountJson(),
        already_cancelled: false,
      }),
    ).toBe('uncertain');
    expect(
      await outcome({
        order: userCancelledOrderJson({ cancel_request_id: 'another-cancel' }),
        account: accountJson(),
        already_cancelled: false,
      }),
    ).toBe('uncertain');
    expect(
      await outcome({
        order: userCancelledOrderJson(),
        account: accountJson({ account_id: 'another-account' }),
        already_cancelled: false,
      }),
    ).toBe('uncertain');
  });

  it('report an order with nothing left to cancel together with its state', async () => {
    const api = createFakeApi();
    api.on('POST', cancelPath, {
      status: 409,
      body: {
        ...errorJson('order_not_cancellable', 'the order has no working remainder to cancel', { field: 'client_order_id' }),
        order: userCancelledOrderJson({ cancel_request_id: 'cancel-9' }),
      },
    });
    const response = await api.client.cancelOrder(ACCOUNT_ID, 'gtc-1', 'cancel-1');
    expect(response.kind).toBe('not_cancellable');
  });

  it('distinguish refusals that changed nothing from uncertain outcomes', async () => {
    const api = createFakeApi();
    const outcome = async (status: number, body: unknown) => {
      api.on('POST', cancelPath, { status, body });
      return (await api.client.cancelOrder(ACCOUNT_ID, 'gtc-1', 'cancel-1')).kind;
    };
    expect(await outcome(409, errorJson('duplicate_cancel_request_id', 'used'))).toBe('refused');
    expect(await outcome(404, errorJson('order_not_found', 'no order'))).toBe('refused');
    expect(await outcome(404, errorJson('account_not_found', 'no account'))).toBe('refused');
    expect(await outcome(400, errorJson('malformed_request', 'bad', { reason: 'invalid_identifier' }))).toBe('refused');
    expect(await outcome(503, errorJson('persistence_unavailable', PERSISTENCE_UNAVAILABLE_MESSAGE))).toBe('uncertain');
    expect(await outcome(500, errorJson('internal_error', 'oops'))).toBe('uncertain');
    expect(await outcome(200, errorJson('duplicate_cancel_request_id', 'used'))).toBe('uncertain');
    expect(await outcome(500, errorJson('order_not_found', 'no order'))).toBe('uncertain');
    // A success that breaks the contract proves nothing.
    expect(await outcome(200, { order: workingOrderJson() })).toBe('uncertain');
    api.failNext('POST', cancelPath);
    expect((await api.client.cancelOrder(ACCOUNT_ID, 'gtc-1', 'cancel-1')).kind).toBe('uncertain');
  });

  it('page working orders and one order\'s lifecycle forward with the server cursor', async () => {
    const api = createFakeApi();
    api.on('GET', `${accountPath}/working-orders`, { status: 200, body: { orders: [workingOrderJson()], next_cursor: 'next-1' } });
    api.on('GET', `${accountPath}/orders/gtc-1/events`, { status: 200, body: { events: [orderEventJson()], next_cursor: null } });
    const page = await api.client.listWorkingOrders(ACCOUNT_ID, 'cursor-0');
    expect(page.next_cursor).toBe('next-1');
    const request = api.requestsTo('GET', `${accountPath}/working-orders`)[0];
    expect(request?.query.get('limit')).toBe(String(FORWARD_PAGE_SIZE));
    expect(request?.query.get('cursor')).toBe('cursor-0');
    const events = await api.client.listOrderEvents(ACCOUNT_ID, 'gtc-1', null);
    expect(events.events).toHaveLength(1);
    expect(api.requestsTo('GET', `${accountPath}/orders/gtc-1/events`)[0]?.query.has('cursor')).toBe(false);
  });
});
