// Words for a deployment's condition, decisions, and timing (ADR 0019).
// Values are reformatted lexically only: times and durations stay the
// server's exact strings, and nothing becomes a JavaScript number.
import type { Condition, Decision, MarketDuration } from '../api/deployments';
import { tradeOutcome } from './research';
import type { Tone } from './vocabulary';

export interface Explained {
  readonly label: string;
  readonly tone: Tone;
  readonly sentence: string;
}

/** Exact milliseconds from integer nanoseconds: "3000000" is "3", "1059" is "0.001059". */
export function nanosecondsAsMilliseconds(nanoseconds: string): string {
  const digits = nanoseconds.padStart(7, '0');
  const whole = digits.slice(0, -6).replace(/^0+(?=\d)/, '');
  const fraction = digits.slice(-6).replace(/0+$/, '');
  return fraction === '' ? whole : `${whole}.${fraction}`;
}

/** Exact milliseconds from integer microseconds: "1234" is "1.234". */
export function microsecondsAsMilliseconds(microseconds: string): string {
  const digits = microseconds.padStart(4, '0');
  const whole = digits.slice(0, -3).replace(/^0+(?=\d)/, '');
  const fraction = digits.slice(-3).replace(/0+$/, '');
  return fraction === '' ? whole : `${whole}.${fraction}`;
}

const UNAVAILABLE_DURATION: Readonly<Record<string, string>> = {
  no_order: 'no order',
  not_resolved: 'not resolved yet',
  no_execution: 'never executed',
  no_fill: 'nothing filled',
  inconsistent_clock: 'unavailable: inconsistent times',
  overflow: 'unavailable: out of range',
};

/** A market-clock duration for reading, or why there is none. */
export function durationText(duration: MarketDuration): string {
  if ('nanoseconds' in duration) {
    return `${nanosecondsAsMilliseconds(duration.nanoseconds)} ms`;
  }
  return UNAVAILABLE_DURATION[duration.unavailable] ?? `unavailable: ${duration.unavailable.replaceAll('_', ' ')}`;
}

const WAITING: Tone = 'caution';

const SOURCE: Readonly<Record<string, string>> = {
  no_market_data_source: 'The server has no market-data source, so nothing can arrive.',
  source_not_started: 'Waiting for market data: the replay has not been started.',
  source_paused: 'Waiting for market data: the replay is paused.',
  source_completed: 'The replay has finished; no more market data will arrive.',
  source_connecting: 'Waiting for market data: the live source is connecting.',
  source_reconnecting: 'Waiting for market data: the live source is reconnecting.',
  source_failed: 'Market data stopped: the source failed.',
  source_stopped: 'Market data stopped.',
};

const QUOTE_UNAVAILABLE: Readonly<Record<string, string>> = {
  no_top_of_book: 'No quote for {instrument} has arrived yet.',
  awaiting_fresh_quote: 'Waiting for a fresh {instrument} quote: the source reconnected, so older quotes are not used.',
  source_unavailable: 'No usable {instrument} quote while the source is unavailable.',
  stream_interrupted: 'The market stream was interrupted; no quote is usable.',
};

const ORDER_WAIT: Readonly<Record<string, string>> = {
  awaiting_latency: 'it waits for a quote received at least the minimum latency after the signal',
  order_matching_busy: 'the market was busy matching when it last tried',
  market_data_unavailable: 'no usable quote is available',
  instrument_not_trading: 'the instrument is not trading',
  suspect_quote: 'the latest quote is flagged as suspect',
  stale_quote: 'the latest quote is too old',
  uncertain: 'the last attempt’s outcome is being checked',
};

const CANDIDATE: Readonly<Record<string, string>> = {
  not_after_watermark: 'The latest quote was already decided on, or is older than one that was.',
  stale_signal: 'The latest quote was older than the maximum signal age by the time it was read.',
  account_unavailable: 'The account could not be read for the latest quote; it tries again on the next.',
  persistence_unavailable: 'The latest decision could not be saved; it tries again on the next quote.',
  decision_refused: 'The latest decision was refused; it tries again on the next quote.',
};

function fill(template: string, instrument: string): string {
  return template.replaceAll('{instrument}', instrument);
}

/**
 * What the deployment is doing now, from the server's condition. Times are
 * left to the caller, which shows them with their clocks.
 */
export function explainCondition(condition: Condition, instrument: string): Explained {
  const { code, reason } = condition;
  switch (code) {
    case 'stopped':
      return { label: 'Stopped', tone: 'neutral', sentence: 'Not trading. It trades again only if started.' };
    case 'stopping':
      return { label: 'Stopping', tone: WAITING, sentence: 'Its runner is ending; nothing new is decided.' };
    case 'faulted':
      return { label: 'Faulted', tone: 'negative', sentence: 'It stopped itself and does not trade until started again.' };
    case 'not_running_here':
      return {
        label: 'Unknown',
        tone: WAITING,
        sentence: 'Recorded as running, but no runner is active in this server process.',
      };
    case 'instrument_not_trading':
      return {
        label: 'Waiting',
        tone: WAITING,
        sentence: `${instrument} is ${reason ?? 'not trading'}; no order is placed until it trades again.`,
      };
    case 'quote_unavailable':
      return {
        label: 'Waiting',
        tone: WAITING,
        sentence: fill(QUOTE_UNAVAILABLE[reason ?? ''] ?? 'No usable {instrument} quote.', instrument),
      };
    case 'quote_suspect':
      return {
        label: 'Waiting',
        tone: WAITING,
        sentence: `The latest ${instrument} quote is flagged as suspect; it waits for a clean one.`,
      };
    case 'quote_stale':
      return {
        label: 'Waiting',
        tone: WAITING,
        sentence: `The latest ${instrument} quote is older than the account’s maximum quote age. No new quote does not mean the market is closed.`,
      };
    case 'order_pending':
      return {
        label: 'Order pending',
        tone: 'info',
        sentence: `An order is waiting to be placed${reason ? `: ${ORDER_WAIT[reason] ?? reason.replaceAll('_', ' ')}` : ''}.`,
      };
    case 'before_training_cutoff':
      return {
        label: 'Waiting',
        tone: WAITING,
        sentence: 'Waiting for the training cutoff: market data received before it is never used.',
      };
    case 'waiting_for_decision_interval':
      return {
        label: 'Waiting',
        tone: 'info',
        sentence: 'The minimum time between decisions has not passed since the latest one.',
      };
    case 'inputs_unavailable':
      return {
        label: 'Waiting',
        tone: WAITING,
        sentence: `The latest quote has no usable model inputs (${(reason ?? 'unavailable').replaceAll('_', ' ')}).`,
      };
    case 'watching':
      return {
        label: 'Watching',
        tone: 'positive',
        sentence:
          reason === 'decided_hold'
            ? 'Its latest prediction was saved as a hold; it waits for the next quote.'
            : reason === 'decided_order'
              ? 'Its latest prediction led to an order; it waits for the next quote.'
              : 'It waits for the next quote.',
      };
    case 'waiting_for_candidate':
      return { label: 'Waiting', tone: 'info', sentence: `No ${instrument} quote has reached it in this run yet.` };
    default:
      return explainOtherCondition(code);
  }
}

// Source conditions, candidate skips, and codes from a newer server.
function explainOtherCondition(code: string): Explained {
  const source = SOURCE[code];
  if (source !== undefined) {
    return { label: 'Waiting', tone: code === 'source_failed' ? 'negative' : WAITING, sentence: source };
  }
  const candidate = CANDIDATE[code];
  if (candidate !== undefined) {
    return { label: 'Waiting', tone: WAITING, sentence: candidate };
  }
  if (code.startsWith('inference_failed:')) {
    return { label: 'Failed', tone: 'negative', sentence: 'The model could not answer.' };
  }
  return { label: 'Unknown', tone: 'neutral', sentence: code.replaceAll('_', ' ') };
}

/** What the policy intended for a saved decision. */
export function decisionIntent(decision: Decision): string {
  if (decision.order === null) {
    return 'Hold';
  }
  return `${decision.order.side === 'buy' ? 'Buy' : 'Sell'} ${decision.order.quantity}`;
}

const POLICY_REASON: Readonly<Record<string, string>> = {
  hold_flat_prediction: 'the prediction was flat',
  hold_below_threshold: 'confidence was below the threshold',
  already_at_target: 'already at the target position',
  order_scheduled: 'to reach the target position',
  order_pending: 'an order was still pending',
};

/** Why the policy intended what it did, for reading after its intent. */
export function policyReasonText(reason: string): string {
  return POLICY_REASON[reason] ?? reason.replaceAll('_', ' ');
}

const ABANDONED: Readonly<Record<string, string>> = {
  signal_expired: 'the signal expired first',
  stop_requested: 'stopped first',
  stopped: 'stopped first',
  kill_switch: 'the kill switch stopped it first',
  server_restarted: 'the server restarted first',
  server_stopped: 'the server stopped first',
  faulted: 'the deployment faulted first',
  superseded_run: 'a newer run began',
};

export interface Outcome {
  readonly label: string;
  readonly tone: Tone;
  // A short addition to the label, or null when the label says it all.
  readonly detail: string | null;
}

/**
 * What became of a saved decision. Only booked fills say something filled:
 * a journaled order can be cancelled or rejected.
 */
export function decisionOutcome(decision: Decision): Outcome {
  if (decision.state === 'held') {
    return { label: 'No order', tone: 'neutral', detail: null };
  }
  if (decision.state === 'pending') {
    return { label: 'Pending', tone: 'info', detail: 'waiting to be placed' };
  }
  const resolution = decision.resolution ?? '';
  if (decision.state === 'abandoned') {
    return { label: 'Never placed', tone: 'caution', detail: ABANDONED[resolution] ?? resolution.replaceAll('_', ' ') };
  }
  const booked = decision.fill;
  if (booked === undefined) {
    // Preserve the server's resolution without claiming booked-fill evidence.
    const outcome = tradeOutcome(resolution);
    if (resolution === 'filled' || resolution === 'partially_filled_and_cancelled') {
      return { label: 'Fill unverified', tone: 'caution', detail: 'fills not reported by this server' };
    }
    return { label: outcome.label, tone: outcome.tone, detail: 'fills not reported by this server' };
  }
  if (booked !== null) {
    const amount = booked.price === null ? `${booked.quantity} in ${booked.count} fills` : `${booked.quantity} @ ${booked.price}`;
    return resolution === 'filled'
      ? { label: 'Filled', tone: 'positive', detail: amount }
      : { label: 'Partly filled', tone: 'caution', detail: `${amount}, rest cancelled` };
  }
  if (resolution.includes('rejected')) {
    return { label: 'Rejected', tone: 'negative', detail: 'nothing filled' };
  }
  return { label: 'Not filled', tone: 'caution', detail: 'nothing executable' };
}

/** The machine code worth showing beside an outcome: one the label does not already say. */
export function outcomeCode(decision: Decision): string | null {
  const resolution = decision.resolution;
  if (resolution === null || resolution === 'filled' || resolution === 'partially_filled_and_cancelled') {
    return null;
  }
  return resolution;
}
