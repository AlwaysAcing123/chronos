import { useQueryClient } from '@tanstack/react-query';
import { useCallback, useMemo, useSyncExternalStore } from 'react';

import { queryKeys } from './queries';

export interface RefreshSummary {
  // Everything currently displayed was confirmed at or after this browser
  // time: the oldest successful update among active reads.
  readonly confirmedAt: number | null;
  // Active reads whose latest attempt failed.
  readonly failingReads: number;
  readonly refreshing: boolean;
}

/** Summarises the polled reads the page is currently showing. */
export function useRefreshSummary(): RefreshSummary {
  const cache = useQueryClient().getQueryCache();
  const subscribe = useCallback((onChange: () => void) => cache.subscribe(onChange), [cache]);
  // A string snapshot keeps useSyncExternalStore's equality check cheap and exact.
  const snapshot = useSyncExternalStore(subscribe, () => {
    const active = cache.findAll({ queryKey: queryKeys.all, type: 'active' });
    const succeeded = active.filter((query) => query.state.status === 'success');
    const confirmedAt = succeeded.length === 0
      ? null
      : Math.min(...succeeded.map((query) => query.state.dataUpdatedAt));
    const failing = active.filter((query) => query.state.status === 'error').length;
    const refreshing = active.some((query) => query.state.fetchStatus === 'fetching');
    return `${String(confirmedAt)}|${String(failing)}|${String(refreshing)}`;
  });
  return useMemo(() => {
    const [confirmedAt = 'null', failing = '0', refreshing = 'false'] = snapshot.split('|');
    return {
      confirmedAt: confirmedAt === 'null' ? null : Number(confirmedAt),
      failingReads: Number(failing),
      refreshing: refreshing === 'true',
    };
  }, [snapshot]);
}
