import { describe, expect, it } from 'vitest';

import type { Condition, Decision } from '../api/deployments';
import type { UnsignedInteger } from '../api/schemas';
import { DecisionSchema } from '../api/deployments';
import { decisionJson } from '../test/deploymentFixtures';
import {
  decisionIntent,
  decisionOutcome,
  durationText,
  explainCondition,
  microsecondsAsMilliseconds,
  nanosecondsAsMilliseconds,
  policyReasonText,
} from './deployments';

function condition(code: string, reason: string | null = null): Condition {
  return {
    code,
    reason,
    observation_received_at: null,
    training_cutoff: null,
    next_eligible_received_at: null,
    not_before_received_at: null,
    expires_at: null,
    decision_sequence: null,
    observed_at_market: null,
    observed_at_wall: null,
  };
}

function decision(overrides: Record<string, unknown> = {}): Decision {
  return DecisionSchema.parse(decisionJson(overrides));
}

describe('exact durations', () => {
  it('reformats integer strings without floating point', () => {
    expect(nanosecondsAsMilliseconds('3000000')).toBe('3');
    expect(nanosecondsAsMilliseconds('1059')).toBe('0.001059');
    expect(nanosecondsAsMilliseconds('0')).toBe('0');
    expect(nanosecondsAsMilliseconds('9007199254740993000000')).toBe('9007199254740993');
    expect(microsecondsAsMilliseconds('1234')).toBe('1.234');
    expect(microsecondsAsMilliseconds('7')).toBe('0.007');
  });

  it('says why a duration is missing instead of showing zero', () => {
    expect(durationText({ clock: 'market', nanoseconds: '1900000' as UnsignedInteger })).toBe('1.9 ms');
    expect(durationText({ clock: 'market', unavailable: 'no_fill' })).toBe('nothing filled');
    expect(durationText({ clock: 'market', unavailable: 'not_resolved' })).toBe('not resolved yet');
    expect(durationText({ clock: 'market', unavailable: 'inconsistent_clock' })).toBe('unavailable: inconsistent times');
    expect(durationText({ clock: 'market', unavailable: 'something_new' })).toBe('unavailable: something new');
  });
});

describe('what a deployment is doing', () => {
  it('explains each condition plainly', () => {
    expect(explainCondition(condition('before_training_cutoff'), 'AAPL').sentence).toMatch(/training cutoff/);
    expect(explainCondition(condition('order_pending', 'awaiting_latency'), 'AAPL').sentence).toMatch(
      /minimum latency after the signal/,
    );
    expect(explainCondition(condition('source_reconnecting', 'reconnecting'), 'AAPL').sentence).toBe(
      'Waiting for market data: the live source is reconnecting.',
    );
    expect(explainCondition(condition('quote_unavailable', 'awaiting_fresh_quote'), 'SPY').sentence).toMatch(
      /fresh SPY quote/,
    );
    expect(explainCondition(condition('instrument_not_trading', 'halted'), 'AAPL').sentence).toMatch(/AAPL is halted/);
    expect(explainCondition(condition('source_failed', 'failed'), 'AAPL').tone).toBe('negative');
    expect(explainCondition(condition('faulted', 'inference_timeout'), 'AAPL').label).toBe('Faulted');
    expect(explainCondition(condition('watching', 'decided_hold'), 'AAPL').label).toBe('Watching');
  });

  it('never reads silence as a closed market', () => {
    expect(explainCondition(condition('quote_stale'), 'AAPL').sentence).toMatch(/does not mean the market is closed/);
  });

  it('keeps an unknown code visible instead of guessing', () => {
    expect(explainCondition(condition('something_new'), 'AAPL')).toEqual({
      label: 'Unknown',
      tone: 'neutral',
      sentence: 'something new',
    });
  });
});

describe('what became of a decision', () => {
  it('says only booked fills filled', () => {
    expect(decisionOutcome(decision())).toEqual({ label: 'Filled', tone: 'positive', detail: '1 @ 768.94' });
    const partial = decision({
      resolution: 'partially_filled_and_cancelled',
      fill: { ...(decisionJson().fill as object), quantity: '0.5', notional: '384.47' },
    });
    expect(decisionOutcome(partial)).toMatchObject({ label: 'Partly filled', detail: '0.5 @ 768.94, rest cancelled' });
    expect(decisionOutcome(decision({ resolution: 'cancelled', fill: null }))).toMatchObject({ label: 'Not filled' });
    expect(decisionOutcome(decision({ resolution: 'risk_rejected:insufficient_cash', fill: null }))).toMatchObject({
      label: 'Rejected',
    });
    // An older server reports no booked-fill evidence.
    const withoutFill = decisionJson();
    delete withoutFill.fill;
    expect(decisionOutcome(DecisionSchema.parse(withoutFill))).toMatchObject({
      label: 'Fill unverified',
      detail: 'fills not reported by this server',
    });
  });

  it('describes holds, pending, and abandoned decisions', () => {
    const held = decision({ state: 'held', order: null, resolution: null, execution: null, fill: null, policy_reason: 'hold_flat_prediction' });
    expect(decisionIntent(held)).toBe('Hold');
    expect(policyReasonText(held.policy_reason)).toBe('the prediction was flat');
    expect(decisionOutcome(held).label).toBe('No order');
    expect(decisionOutcome(decision({ state: 'pending', resolution: null, execution: null, fill: null })).label).toBe('Pending');
    expect(
      decisionOutcome(decision({ state: 'abandoned', resolution: 'signal_expired', execution: null, fill: null })),
    ).toMatchObject({ label: 'Never placed', detail: 'the signal expired first' });
    expect(decisionIntent(decision())).toBe('Buy 1');
  });
});
