import { describe, expect, it } from 'vitest';

import { buildOrderRequest, INITIAL_DRAFT, validateOrderDraft } from './orderDraft';

describe('order draft', () => {
  it('requires a limit price only for limit orders', () => {
    expect(validateOrderDraft({ ...INITIAL_DRAFT, quantity: '10' })).toEqual({});
    expect(validateOrderDraft({ ...INITIAL_DRAFT, type: 'limit', quantity: '10' })).toEqual({
      limitPrice: 'Required.',
    });
    expect(validateOrderDraft({ ...INITIAL_DRAFT, type: 'limit', quantity: '10', limitPrice: '337.5' })).toEqual({});
  });

  it('checks quantity and price scales separately', () => {
    const draft = { ...INITIAL_DRAFT, type: 'limit' as const, quantity: '0.000000001', limitPrice: '1.000000001' };
    expect(validateOrderDraft(draft)).toEqual({ quantity: 'Use at most 8 decimal places.' });
  });

  it('builds a market request without a limit price, even if one was typed earlier', () => {
    const request = buildOrderRequest({ ...INITIAL_DRAFT, quantity: '10', limitPrice: '337' }, 'AAPL', 'web-1');
    expect(request).toEqual({
      client_order_id: 'web-1',
      source: { kind: 'manual', id: 'web-ticket' },
      instrument: 'AAPL',
      side: 'buy',
      type: 'market',
      quantity: '10',
      time_in_force: 'ioc',
    });
    expect('limit_price' in request).toBe(false);
  });

  it('sends decimals exactly as typed', () => {
    const request = buildOrderRequest(
      { side: 'sell', type: 'limit', quantity: '1.50', limitPrice: '300.000', timeInForce: 'ioc' },
      'AAPL',
      'web-2',
    );
    expect(request).toMatchObject({ quantity: '1.50', limit_price: '300.000', side: 'sell', type: 'limit' });
  });

  it('sends the chosen time in force for a limit order', () => {
    const request = buildOrderRequest(
      { side: 'buy', type: 'limit', quantity: '10', limitPrice: '100', timeInForce: 'gtc' },
      'AAPL',
      'web-3',
    );
    expect(request).toMatchObject({ type: 'limit', limit_price: '100', time_in_force: 'gtc' });
  });

  it('forces a market order to immediate-or-cancel even after GTC was chosen for a limit', () => {
    const request = buildOrderRequest(
      { side: 'buy', type: 'market', quantity: '10', limitPrice: '100', timeInForce: 'gtc' },
      'AAPL',
      'web-4',
    );
    expect(request).toMatchObject({ type: 'market', time_in_force: 'ioc' });
    expect('limit_price' in request).toBe(false);
  });
});
