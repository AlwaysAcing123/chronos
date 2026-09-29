import { describe, expect, it } from 'vitest';

import {
  accountJson,
  cancelledOrderJson,
  fillJson,
  healthJson,
  marketDataJson,
  partialOrderJson,
  portfolioJson,
  rejectedOrderJson,
  liveSourceJson,
  replaySourceJson,
} from '../test/fixtures';
import {
  AccountSchema,
  FillPageSchema,
  HealthSchema,
  MarketDataSchema,
  OrderSchema,
  PortfolioSchema,
} from './schemas';

describe('response decoders', () => {
  it('keep every financial value as the exact server string', () => {
    const portfolio = PortfolioSchema.parse(portfolioJson());
    expect(portfolio.equity).toBe('99994.58');
    expect(portfolio.unrealized_pnl).toBe('-2.95');
    expect(portfolio.return_percent).toBe('-0.00542');
    expect(portfolio.market.bid).toEqual({ price: '335.82', quantity: '80' });
    expect(portfolio.market.ingest_sequence).toBe('121297');
    for (const value of [portfolio.equity, portfolio.cash, portfolio.mark_price, portfolio.position]) {
      expect(typeof value).toBe('string');
    }
  });

  it('preserve values beyond double precision', () => {
    const account = AccountSchema.parse(
      accountJson({ cash: '92233720368.54775807', version: '9223372036854775807' }),
    );
    expect(account.cash).toBe('92233720368.54775807');
    expect(account.version).toBe('9223372036854775807');
  });

  it('reject JSON numbers where the contract promises strings', () => {
    expect(PortfolioSchema.safeParse(portfolioJson({ equity: 99994.58 })).success).toBe(false);
    expect(AccountSchema.safeParse(accountJson({ version: 2 })).success).toBe(false);
    expect(OrderSchema.safeParse(partialOrderJson({ quantity: 10 })).success).toBe(false);
  });

  it('reject non-canonical decimals and malformed timestamps', () => {
    expect(PortfolioSchema.safeParse(portfolioJson({ cash: '99155.030' })).success).toBe(false);
    expect(PortfolioSchema.safeParse(portfolioJson({ valued_at: '2026-09-25T13:39:59Z' })).success).toBe(false);
    expect(PortfolioSchema.safeParse(portfolioJson({ mark_price: '335.8200000001' })).success).toBe(false);
  });

  it('refuse any resource that is not paper trading', () => {
    expect(HealthSchema.safeParse(healthJson({ trading_mode: 'live' })).success).toBe(false);
    expect(AccountSchema.safeParse(accountJson({ trading_mode: 'live' })).success).toBe(false);
    expect(OrderSchema.safeParse(partialOrderJson({ trading_mode: 'live' })).success).toBe(false);
  });

  it('refuse an unknown mark policy or order status', () => {
    expect(PortfolioSchema.safeParse(portfolioJson({ mark_policy: 'midpoint' })).success).toBe(false);
    expect(OrderSchema.safeParse(partialOrderJson({ status: 'working' })).success).toBe(false);
  });

  it('enforce the order-outcome invariants the interface relies on', () => {
    expect(OrderSchema.safeParse(partialOrderJson()).success).toBe(true);
    expect(OrderSchema.safeParse(cancelledOrderJson()).success).toBe(true);
    expect(OrderSchema.safeParse(rejectedOrderJson()).success).toBe(true);
    // A filled order without its fill, or a rejection without a reason object.
    expect(OrderSchema.safeParse(partialOrderJson({ fill: null })).success).toBe(false);
    expect(OrderSchema.safeParse(rejectedOrderJson({ rejection: null })).success).toBe(false);
    expect(OrderSchema.safeParse(cancelledOrderJson({ cancellation_reason: null })).success).toBe(false);
  });

  it('require an interrupted stream to explain itself', () => {
    const interrupted = healthJson({
      market_data: {
        stream_id: 's',
        state: 'interrupted',
        last_ingest_sequence: '3',
        market_time: null,
        interruption: null,
      },
    });
    expect(HealthSchema.safeParse(interrupted).success).toBe(false);
  });

  it('ignore unknown keys so additive server changes stay compatible', () => {
    const page = FillPageSchema.parse({ fills: [fillJson({ added_later: 'x' })], next_cursor: null, extra: 1 });
    expect(page.fills[0]).not.toHaveProperty('added_later');
  });

  it('decode market data with exact counts and refuse contradictions', () => {
    const decoded = MarketDataSchema.parse(
      marketDataJson({ source: replaySourceJson({ events_applied: '18446744073709551615' }) }),
    );
    expect(decoded.source.events_applied).toBe('18446744073709551615');
    expect(decoded.revision).toBe('7');
    // A live source has no playback speed; an unavailable quote says why.
    expect(
      MarketDataSchema.safeParse(marketDataJson({ source: replaySourceJson({ mode: 'live' }) })).success,
    ).toBe(false);
    expect(
      MarketDataSchema.safeParse({
        ...marketDataJson(),
        instruments: [{ instrument: 'AAPL', execution_quote_available: false, unavailable_reason: null }],
      }).success,
    ).toBe(false);
    expect(
      MarketDataSchema.safeParse(marketDataJson({ source: replaySourceJson({ events_applied: 5 }) })).success,
    ).toBe(false);
    expect(
      MarketDataSchema.safeParse(marketDataJson({ source: replaySourceJson({ state: 'rewinding' }) })).success,
    ).toBe(false);
    expect(
      MarketDataSchema.safeParse(marketDataJson({ source: replaySourceJson({ recording_sha256: 'abc' }) })).success,
    ).toBe(false);
  });

  it('decode a live source with its coverage notice and live unavailability reasons', () => {
    const decoded = MarketDataSchema.parse(
      marketDataJson({
        source: liveSourceJson({ state: 'reconnecting', diagnostic: 'the connection closed' }),
        reasons: { AAPL: 'source_unavailable', SPY: 'awaiting_fresh_quote' },
      }),
    );
    expect(decoded.source.label).toBe('Alpaca IEX');
    expect(decoded.source.coverage_notice).toMatch(/not the NBBO/);
    expect(decoded.source.replay_speed).toBeNull();
    expect(decoded.instruments.map((item) => item.unavailable_reason)).toEqual([
      'source_unavailable',
      'awaiting_fresh_quote',
    ]);
    // A live source cannot claim a playback speed.
    expect(MarketDataSchema.safeParse(marketDataJson({ source: liveSourceJson({ replay_speed: '1x' }) })).success).toBe(
      false,
    );
  });
});
