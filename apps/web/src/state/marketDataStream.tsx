// The server's change stream (Server-Sent Events). A notice carries no market
// data: it only says the market or source changed, so the provider
// invalidates the market-dependent queries and they refetch their typed
// resources. Without an open stream, those queries poll instead.
import { useQueryClient } from '@tanstack/react-query';
import type { ReactNode } from 'react';
import { useEffect, useState } from 'react';

import { isMarketDependentQuery, queryKeys } from './queries';
import { useChronosServices } from './services';
import type { StreamStatus } from './streamStatus';
import { StreamStatusContext } from './streamStatus';

// The part of EventSource the provider uses, so tests can supply a fake.
export interface NoticeStream {
  readonly readyState: number;
  onopen: ((event: Event) => void) | null;
  onerror: ((event: Event) => void) | null;
  addEventListener(type: 'market-data', listener: (event: MessageEvent) => void): void;
  close(): void;
}

export type OpenNoticeStream = () => NoticeStream;

// The market-data resource and the resources valued against it.
function isMarketQuery(key: readonly unknown[]): boolean {
  const marketData = queryKeys.marketData();
  return (key[0] === marketData[0] && key[1] === marketData[1] && key.length === 2) || isMarketDependentQuery(key);
}

// EventSource.CLOSED: the browser will not reconnect by itself.
const CLOSED = 2;
const RECONNECT_DELAYS_MS = [2_000, 5_000, 10_000, 30_000] as const;

export function MarketDataStreamProvider({ children }: { readonly children: ReactNode }) {
  const { openNoticeStream } = useChronosServices();
  const queryClient = useQueryClient();
  const [status, setStatus] = useState<StreamStatus>(openNoticeStream ? 'connecting' : 'polling');

  useEffect(() => {
    if (!openNoticeStream) {
      return undefined;
    }
    let stream: NoticeStream | null = null;
    let reconnect: ReturnType<typeof setTimeout> | undefined;
    let failures = 0;
    const cache = queryClient.getQueryCache();
    // An in-flight read completes rather than restarting, so a burst of
    // notices cannot starve it. It may have been answered before the change
    // it was told about, so each query that was fetching when a notice
    // arrived is read once more when it settles; the latest change is never
    // left waiting for the safety poll.
    const keepInFlight = { cancelRefetch: false };
    const behind = new Set<string>();

    const refreshMarket = () => {
      for (const query of cache.findAll({ predicate: (candidate) => isMarketQuery(candidate.queryKey) })) {
        if (query.state.fetchStatus === 'fetching') {
          behind.add(query.queryHash);
        }
      }
      void queryClient.invalidateQueries({ predicate: (query) => isMarketQuery(query.queryKey) }, keepInFlight);
    };

    const unsubscribe = cache.subscribe((event) => {
      const hash = event.query.queryHash;
      if (event.type !== 'updated' || event.query.state.fetchStatus !== 'idle' || !behind.delete(hash)) {
        return;
      }
      void queryClient.invalidateQueries({ predicate: (query) => query.queryHash === hash }, keepInFlight);
    });

    const connect = () => {
      const opened = openNoticeStream();
      stream = opened;
      opened.onopen = () => {
        failures = 0;
        setStatus('streaming');
        // Changes may have happened while disconnected.
        refreshMarket();
      };
      opened.addEventListener('market-data', refreshMarket);
      opened.onerror = () => {
        setStatus('polling');
        // The browser retries a dropped stream itself; a refused one it
        // abandons, so reconnect with growing delays while polling.
        if (opened.readyState === CLOSED) {
          opened.close();
          const delay = RECONNECT_DELAYS_MS[Math.min(failures, RECONNECT_DELAYS_MS.length - 1)];
          failures += 1;
          reconnect = setTimeout(connect, delay);
        }
      };
    };

    connect();
    return () => {
      clearTimeout(reconnect);
      stream?.close();
      unsubscribe();
    };
  }, [openNoticeStream, queryClient]);

  return <StreamStatusContext value={status}>{children}</StreamStatusContext>;
}
