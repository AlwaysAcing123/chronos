// Words for strategy diagnostics. Every figure is the service's exact string;
// these only name things. Each verification level has its own words, so no
// single badge ever stands for all of them.
import type {
  CohortName,
  Component,
  DiagnosticsVerification,
  SimulatedAccount,
  SourceKind,
  Trust,
} from '../api/diagnosticsSchemas';
import { SOURCE_KINDS } from '../api/diagnosticsSchemas';
import type { Labelled } from './vocabulary';

export const ACCOUNT_LABEL: Readonly<Record<SimulatedAccount, string>> = {
  strategy: 'Strategy',
  buy_and_hold: 'Buy and hold',
  cash: 'Cash',
};

export const COMPONENT_LABEL: Readonly<Record<Component, string>> = {
  booked_net_pnl: 'Booked net PnL',
  midpoint_inventory_pnl: 'Midpoint inventory PnL',
  execution_spread_cost: 'Spread paid at execution',
  fees: 'Fees',
  terminal_bid_markdown: 'Final mark at the bid, not the midpoint',
  rounding_adjustment: 'Rounding adjustment',
  market_value_rounding: 'Market value rounding',
  notional_rounding: 'Booked notional rounding',
  observed_midpoint_movement: 'Midpoint move, signal to execution',
  signal_reference_pnl: 'PnL at signal midpoints',
  inventory_pnl_at_bid: 'Inventory PnL at the final bid',
};

// The identity, in order: booked = inventory − spread − fees − markdown + rounding.
export const BRIDGE: readonly { readonly component: Component; readonly operator: string }[] = [
  { component: 'midpoint_inventory_pnl', operator: '' },
  { component: 'execution_spread_cost', operator: '−' },
  { component: 'fees', operator: '−' },
  { component: 'terminal_bid_markdown', operator: '−' },
  { component: 'rounding_adjustment', operator: '+' },
  { component: 'booked_net_pnl', operator: '=' },
];

// The second identity: inventory = signal reference − movement.
export const SIGNAL_BRIDGE: readonly { readonly component: Component; readonly operator: string }[] = [
  { component: 'signal_reference_pnl', operator: '' },
  { component: 'observed_midpoint_movement', operator: '−' },
  { component: 'midpoint_inventory_pnl', operator: '=' },
];

// The columns of the overview tables: every term of both identities.
export const OVERVIEW_COMPONENTS: readonly Component[] = [
  'booked_net_pnl',
  'midpoint_inventory_pnl',
  'execution_spread_cost',
  'fees',
  'terminal_bid_markdown',
  'rounding_adjustment',
  'observed_midpoint_movement',
  'signal_reference_pnl',
];

export const COHORT_LABEL: Readonly<Record<CohortName, string>> = {
  scorable_predictions: 'Scorable predictions',
  sampled_decisions: 'Sampled decisions',
  order_scheduling_decisions: 'Order-scheduling decisions',
  filled_orders: 'Orders with fills',
};

export const POOL_STATUS: Readonly<Record<'complete' | 'partial' | 'unavailable', Labelled>> = {
  complete: { label: 'All folds', tone: 'positive' },
  partial: { label: 'Partial', tone: 'caution' },
  unavailable: { label: 'No fold', tone: 'neutral' },
};

export const RECONCILIATION: Readonly<Record<'reconciled' | 'reconciled_at_bid', Labelled>> = {
  reconciled: { label: 'Reconciles exactly', tone: 'positive' },
  reconciled_at_bid: { label: 'Reconciles exactly at the bid', tone: 'positive' },
};

// What an identity's state can claim when the figures' sources did not
// verify on this read: only that it held when the diagnostics were built.
export const RECORDED_RECONCILIATION: Readonly<Record<'reconciled' | 'reconciled_at_bid', Labelled>> = {
  reconciled: { label: 'Reconciled when generated', tone: 'neutral' },
  reconciled_at_bid: { label: 'Reconciled at the bid when generated', tone: 'neutral' },
};

export const SOURCE_LABEL: Readonly<Record<SourceKind, { readonly one: string; readonly many: string }>> = {
  studies: { one: 'Study', many: 'Studies' },
  plans: { one: 'Plan', many: 'Plans' },
  collections: { one: 'Collection', many: 'Collections' },
  datasets: { one: 'Dataset', many: 'Datasets' },
  features: { one: 'Feature artifact', many: 'Feature artifacts' },
  labels: { one: 'Label artifact', many: 'Label artifacts' },
  schedules: { one: 'Schedule', many: 'Schedules' },
  runs: { one: 'Run journal', many: 'Run journals' },
};

/** A figure is trusted only if the read verified every source it rests on;
 * a figure the read says nothing about is not trusted. */
export function trusted(trust: Trust | undefined): boolean {
  return trust?.state === 'verified';
}

/** The sources a figure rests on that did not verify, in words. */
export function untrustedSources(trust: Trust | undefined): readonly string[] {
  if (trust === undefined) {
    return ['Not checked by this read'];
  }
  if (trust.state === 'verified') {
    return [];
  }
  return trust.sources.map((key) => {
    const [kind = '', identifier = ''] = key.split(':');
    const label = SOURCE_KINDS.find((item) => item === kind);
    return `${label === undefined ? kind : SOURCE_LABEL[label].one} ${identifier.slice(0, 12)}`;
  });
}

const WORDS: Readonly<Record<string, string>> = {
  final_valuation_unavailable: 'The final valuation is unavailable',
  final_midpoint_unavailable: 'The final quote has no midpoint',
  execution_midpoint_unavailable: 'An execution quote has no midpoint',
  signal_midpoint_unavailable: 'A signal quote has no midpoint',
  midpoint_unavailable: 'A quote has no midpoint',
  bid_missing: 'no bid',
  ask_missing: 'no ask',
  position_mark_unavailable: 'the position could not be marked',
  no_executions: 'No order executed',
  no_completed_episodes: 'No holding episode completed',
  no_fills_with_both_sides: 'No fill had both sides quoted',
  no_scored_items: 'Nothing scored',
  no_completed_runs: 'No run of this pool completed, so nothing was evaluated',
  model_unevaluable: 'Its model could not be fitted',
  model_failed: 'Its model failed',
  model_unavailable: 'No model',
  cell_failed: 'Its run failed',
  cell_unavailable: 'It was not run',
  run_failed: 'the strategy run failed',
  insufficient_class_support: 'too few training rows of some class',
};

/** A reason code in words, keeping the code beside it in the page. */
export function diagnosticReason(code: string): string {
  const parts = code.split(':');
  const words = parts.map((part, index) => {
    if (/^fold_[0-9]+$/.test(part)) {
      return `fold ${part.slice(5)}`;
    }
    const known = WORDS[part];
    if (known !== undefined) {
      return index === 0 ? known : known.charAt(0).toLowerCase() + known.slice(1);
    }
    return part.replaceAll('_', ' ');
  });
  const [head, ...rest] = words;
  return rest.length === 0 ? (head ?? code) : `${head ?? ''}: ${rest.join(', ')}`;
}

export interface Level {
  readonly name: string;
  readonly state: Labelled;
  readonly detail: string;
  readonly command?: string;
}

/** The verification levels, each named apart: what this read checked now,
 * what generation checked, and what only the command line does. */
export function verificationLevels(verification: DiagnosticsVerification): readonly Level[] {
  const now = verification.sources.now;
  return [
    {
      name: 'Artifact checksums',
      state: { label: 'Verified on this load', tone: 'positive' },
      detail: verification.artifact.detail,
    },
    {
      name: 'Every source, checked on this read',
      state:
        now.state === 'verified'
          ? { label: 'Match what was read', tone: 'positive' }
          : { label: 'Do not match', tone: 'negative' },
      detail: `${now.detail}. On this read: ${now.content.hashed_now} files hashed now and ${now.content.reused_unchanged} reused unchanged`,
    },
    {
      name: 'Sources at generation',
      state: { label: 'Recorded when generated', tone: 'neutral' },
      detail: verification.sources.at_generation,
    },
    {
      name: 'Recomputed inspection',
      state: { label: 'Not run by this page', tone: 'neutral' },
      detail: verification.recomputed_inspection.detail,
      command: verification.recomputed_inspection.command,
    },
    {
      name: 'Independent arithmetic audit',
      state: { label: 'Not run by this page', tone: 'neutral' },
      detail: verification.independent_audit.detail,
      command: verification.independent_audit.command,
    },
    {
      name: 'Engine replay',
      state: { label: 'Recorded by the study', tone: 'neutral' },
      detail: verification.engine_replay.detail,
    },
  ];
}

/** Every source that does not match, in words. */
export function sourceProblems(verification: DiagnosticsVerification): readonly string[] {
  const sources = verification.sources.now.sources;
  return SOURCE_KINDS.flatMap((kind) =>
    Object.entries(sources[kind])
      .filter(([, state]) => state !== 'verified')
      .map(([identifier, state]) => `${SOURCE_LABEL[kind].one} ${identifier.slice(0, 12)}: ${state}`),
  );
}

/** Each kind of source: how many the read checked, and how many verified. */
export function sourceCounts(
  verification: DiagnosticsVerification,
): readonly { readonly kind: SourceKind; readonly checked: number; readonly verified: number }[] {
  const sources = verification.sources.now.sources;
  return SOURCE_KINDS.map((kind) => {
    const states = Object.values(sources[kind]);
    return { kind, checked: states.length, verified: states.filter((state) => state === 'verified').length };
  });
}

export const STAGE: Readonly<Record<string, string>> = {
  verify_sources: 'Verifying the studies’ sources',
  attribute: 'Attributing runs',
  aggregate: 'Pooling over folds',
  publish: 'Publishing',
};
