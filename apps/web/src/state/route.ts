// The page's view, kept in the URL hash so a reload or a shared link opens
// the same view. Trading is the default; its in-page section links (such as
// #portfolio) keep working because only #/research... selects Research. The
// account selection is separate browser state, so switching views never
// changes it.
import { useCallback, useEffect, useState } from 'react';

export type Route =
  | { readonly view: 'trading' }
  | { readonly view: 'research'; readonly resultId: string | null };

const RESULT = /^#\/research\/results\/([0-9a-f]{64})$/;

export function parseRoute(hash: string): Route {
  const result = RESULT.exec(hash);
  if (result !== null) {
    return { view: 'research', resultId: result[1] ?? null };
  }
  if (hash === '#/research' || hash.startsWith('#/research/')) {
    return { view: 'research', resultId: null };
  }
  return { view: 'trading' };
}

export function researchHref(resultId: string | null = null): string {
  return resultId === null ? '#/research' : `#/research/results/${resultId}`;
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
