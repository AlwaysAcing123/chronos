// The page's view, kept in the URL hash so a reload or a shared link opens
// the same view. Trading is the default; its in-page section links (such as
// #portfolio) keep working because only #/research... selects Research and
// only #/models... the model library and #/comparisons... its comparisons.
// Walk-forward evaluation lives inside Research, under #/research/walk-forward,
// and so do strategy diagnostics, whose deep link names one run and one of its
// simulated accounts. The paper-trading account selection is separate browser
// state, so switching views, runs, or simulated accounts never changes it.
// Only a demo workspace's build has #/demo, its guide (ADR 0025); elsewhere
// that hash opens trading, as any unknown hash does.
import { useCallback, useEffect, useState } from 'react';

import { DEMO_ENABLED } from './demo';

export type Route =
  | { readonly view: 'trading' }
  | { readonly view: 'demo' }
  | { readonly view: 'research'; readonly resultId: string | null }
  | { readonly view: 'models'; readonly modelId: string | null }
  | { readonly view: 'comparisons'; readonly comparisonId: string | null }
  | { readonly view: 'walk-forward'; readonly planId: string | null; readonly studyId: string | null }
  | {
      readonly view: 'diagnostics';
      readonly diagnosticId: string;
      readonly runId: string | null;
      readonly account: SimulatedAccount;
    };

// The simulated accounts of a strategy run: never a paper-trading account.
export type SimulatedAccount = 'strategy' | 'buy_and_hold' | 'cash';
const SIMULATED: readonly SimulatedAccount[] = ['strategy', 'buy_and_hold', 'cash'];

const RESULT = /^#\/research\/results\/([0-9a-f]{64})$/;
const MODEL = /^#\/models\/([0-9a-f]{64})$/;
const COMPARISON = /^#\/comparisons\/([0-9a-f]{64})$/;
const WALK_FORWARD = '#/research/walk-forward';
const PLAN = /^#\/research\/walk-forward\/plans\/([0-9a-f]{64})$/;
const STUDY = /^#\/research\/walk-forward\/studies\/([0-9a-f]{64})$/;
const DIAGNOSTICS =
  /^#\/research\/walk-forward\/diagnostics\/([0-9a-f]{64})(?:\/runs\/([0-9a-f]{64})(?:\/(strategy|buy_and_hold|cash))?)?$/;

export const DEMO_HREF = '#/demo';

export function parseRoute(hash: string, demo: boolean = DEMO_ENABLED): Route {
  if (demo && (hash === DEMO_HREF || hash.startsWith(`${DEMO_HREF}/`))) {
    return { view: 'demo' };
  }
  const plan = PLAN.exec(hash);
  if (plan !== null) {
    return { view: 'walk-forward', planId: plan[1] ?? null, studyId: null };
  }
  const diagnostics = DIAGNOSTICS.exec(hash);
  if (diagnostics?.[1] !== undefined) {
    const account = SIMULATED.find((name) => name === diagnostics[3]) ?? 'strategy';
    return { view: 'diagnostics', diagnosticId: diagnostics[1], runId: diagnostics[2] ?? null, account };
  }
  const study = STUDY.exec(hash);
  if (study !== null) {
    return { view: 'walk-forward', planId: null, studyId: study[1] ?? null };
  }
  // Anything else under it opens its home, never a path.
  if (hash === WALK_FORWARD || hash.startsWith(`${WALK_FORWARD}/`)) {
    return { view: 'walk-forward', planId: null, studyId: null };
  }
  const result = RESULT.exec(hash);
  if (result !== null) {
    return { view: 'research', resultId: result[1] ?? null };
  }
  if (hash === '#/research' || hash.startsWith('#/research/')) {
    return { view: 'research', resultId: null };
  }
  const model = MODEL.exec(hash);
  if (model !== null) {
    return { view: 'models', modelId: model[1] ?? null };
  }
  if (hash === '#/models' || hash.startsWith('#/models/')) {
    return { view: 'models', modelId: null };
  }
  const comparison = COMPARISON.exec(hash);
  if (comparison !== null) {
    return { view: 'comparisons', comparisonId: comparison[1] ?? null };
  }
  if (hash === '#/comparisons' || hash.startsWith('#/comparisons/')) {
    return { view: 'comparisons', comparisonId: null };
  }
  return { view: 'trading' };
}

export function researchHref(resultId: string | null = null): string {
  return resultId === null ? '#/research' : `#/research/results/${resultId}`;
}

export function modelsHref(modelId: string | null = null): string {
  return modelId === null ? '#/models' : `#/models/${modelId}`;
}

export function comparisonsHref(comparisonId: string | null = null): string {
  return comparisonId === null ? '#/comparisons' : `#/comparisons/${comparisonId}`;
}

export function walkForwardHref(target: { readonly planId?: string; readonly studyId?: string } = {}): string {
  if (target.studyId !== undefined) {
    return `${WALK_FORWARD}/studies/${target.studyId}`;
  }
  return target.planId === undefined ? WALK_FORWARD : `${WALK_FORWARD}/plans/${target.planId}`;
}

export function diagnosticsHref(target: {
  readonly diagnosticId: string;
  readonly runId?: string | null;
  readonly account?: SimulatedAccount;
}): string {
  const base = `${WALK_FORWARD}/diagnostics/${target.diagnosticId}`;
  if (target.runId === undefined || target.runId === null) {
    return base;
  }
  return `${base}/runs/${target.runId}/${target.account ?? 'strategy'}`;
}

export const TRADING_HREF = '#/';

export function useRoute(): Route {
  const [route, setRoute] = useState<Route>(() => parseRoute(window.location.hash));
  const update = useCallback(() => {
    setRoute(parseRoute(window.location.hash));
  }, []);
  useEffect(() => {
    window.addEventListener('hashchange', update);
    return () => {
      window.removeEventListener('hashchange', update);
    };
  }, [update]);
  return route;
}
