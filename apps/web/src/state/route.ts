// The page's view, kept in the URL hash so a reload or a shared link opens
// the same view. Trading is the default; its in-page section links (such as
// #portfolio) keep working because only #/research... selects Research and
// only #/models... the model library and #/comparisons... its comparisons.
// The account selection is separate browser state, so switching views never
// changes it.
import { useCallback, useEffect, useState } from 'react';

export type Route =
  | { readonly view: 'trading' }
  | { readonly view: 'research'; readonly resultId: string | null }
  | { readonly view: 'models'; readonly modelId: string | null }
  | { readonly view: 'comparisons'; readonly comparisonId: string | null };

const RESULT = /^#\/research\/results\/([0-9a-f]{64})$/;
const MODEL = /^#\/models\/([0-9a-f]{64})$/;
const COMPARISON = /^#\/comparisons\/([0-9a-f]{64})$/;

export function parseRoute(hash: string): Route {
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
