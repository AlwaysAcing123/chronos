// Words for the research service's vocabulary. Values are reformatted
// lexically only; nothing financial becomes a JavaScript number.
import type { AccountName, JobState, ModelName, WindowName } from '../api/researchSchemas';
import type { Labelled } from './vocabulary';

export const JOB_STATE: Readonly<Record<JobState, Labelled>> = {
  queued: { label: 'Queued', tone: 'neutral' },
  running: { label: 'Running', tone: 'info' },
  succeeded: { label: 'Succeeded', tone: 'positive' },
  failed: { label: 'Failed', tone: 'negative' },
  interrupted: { label: 'Interrupted', tone: 'caution' },
};

export const ACCOUNT_LABEL: Readonly<Record<AccountName, string>> = {
  strategy: 'Strategy',
  buy_and_hold: 'Buy and hold',
  cash: 'Cash',
};

export const WINDOW_LABEL: Readonly<Record<WindowName, string>> = {
  validation: 'Validation',
  test: 'Test',
};

export const MODEL_LABEL: Readonly<Record<ModelName, string>> = {
  logistic: 'Logistic regression',
  majority: 'Majority class (no skill)',
};

const DECISION_REASON: Readonly<Record<string, string>> = {
  order_scheduled: 'Order scheduled',
  already_at_target: 'Already at target',
  hold_below_threshold: 'Hold: below threshold',
  hold_flat_prediction: 'Hold: flat prediction',
  order_pending: 'Hold: an order is pending',
  benchmark_entry: 'Benchmark entry',
};

export function decisionReason(reason: string): string {
  return DECISION_REASON[reason] ?? reason.replaceAll('_', ' ');
}

const OUTCOME: Readonly<Record<string, Labelled>> = {
  filled: { label: 'Filled', tone: 'positive' },
  partially_filled_and_cancelled: { label: 'Partially filled, rest cancelled', tone: 'caution' },
  cancelled: { label: 'Cancelled', tone: 'neutral' },
  rejected: { label: 'Rejected', tone: 'negative' },
  expired: { label: 'Expired at window end', tone: 'caution' },
};

export function tradeOutcome(outcome: string): Labelled {
  return OUTCOME[outcome] ?? { label: outcome, tone: 'neutral' };
}

/** Parts per billion as an exact percentage: "500000000" is "50", "501539269" is "50.1539269". */
export function ppbAsPercent(ppb: string): string {
  const digits = ppb.padStart(8, '0');
  const whole = digits.slice(0, -7).replace(/^0+(?=\d)/, '');
  const fraction = digits.slice(-7).replace(/0+$/, '');
  return fraction === '' ? whole : `${whole}.${fraction}`;
}

/** A model metric's value for reading, with the exact value kept for its title. */
export function metricPercent(value: string): string {
  // Classification metrics are not financial values; two decimals of a
  // percentage are shown and the exact value is in the title.
  const number = Number(value);
  return Number.isFinite(number) ? `${(number * 100).toFixed(2)}%` : value;
}

/** The first and last characters of a long ID. */
export function shortId(id: string): string {
  return `${id.slice(0, 12)}…`;
}
