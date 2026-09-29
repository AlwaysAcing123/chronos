// Server state hooks. Components read resources only through these hooks, so
// replacing polling with a live subscription changes this file, not them.
import type { InfiniteData } from '@tanstack/react-query';
import { useInfiniteQuery, useQuery } from '@tanstack/react-query';

import type { FillPage, OrderPage } from '../api/schemas';
import { isAccountNotFound } from '../api/errors';
import type { RefreshPolicy } from './refreshPolicy';
import { useChronosServices } from './services';
import { useStreamStatus } from './streamStatus';

export const queryKeys = {
  all: ['chronos'] as const,
  health: () => [...queryKeys.all, 'health'] as const,
  marketData: () => [...queryKeys.all, 'market-data'] as const,
  account: (accountId: string) => [...queryKeys.all, 'accounts', accountId] as const,
  accountResource: (accountId: string) => [...queryKeys.account(accountId), 'resource'] as const,
  portfolio: (accountId: string) => [...queryKeys.account(accountId), 'portfolio'] as const,
  // One history per symbol filter; null is every symbol.
  orders: (accountId: string, instrument: string | null) =>
    [...queryKeys.account(accountId), 'orders', instrument ?? '*'] as const,
  fills: (accountId: string, instrument: string | null) =>
    [...queryKeys.account(accountId), 'fills', instrument ?? '*'] as const,
};

// Resources whose value depends on the current market, refreshed by the
// change stream: the portfolio valuations. Market data has its own key.
export function isMarketDependentQuery(key: readonly unknown[]): boolean {
  return key[0] === queryKeys.all[0] && key[1] === 'accounts' && key[3] === 'portfolio';
}

// Stops polling an account the server says does not exist.
function accountInterval(interval: number | false) {
  return (query: { readonly state: { readonly error: unknown } }) =>
    isAccountNotFound(query.state.error) ? false : interval;
}

/** How often market-dependent reads poll: a safety net while the change stream is open. */
export function marketInterval(refresh: RefreshPolicy, streaming: boolean): number | false {
  return streaming ? refresh.streamedMarketMs : refresh.marketMs;
}

export function useHealth() {
  const { client, refresh } = useChronosServices();
  return useQuery({
    queryKey: queryKeys.health(),
    queryFn: ({ signal }) => client.getHealth(signal),
    refetchInterval: refresh.healthMs,
  });
}

export function useMarketData() {
  const { client, refresh } = useChronosServices();
  const streaming = useStreamStatus() === 'streaming';
  return useQuery({
    queryKey: queryKeys.marketData(),
    queryFn: ({ signal }) => client.getMarketData(signal),
    refetchInterval: marketInterval(refresh, streaming),
  });
}

export function useAccount(accountId: string) {
  const { client, refresh } = useChronosServices();
  return useQuery({
    queryKey: queryKeys.accountResource(accountId),
    queryFn: ({ signal }) => client.getAccount(accountId, signal),
    refetchInterval: accountInterval(refresh.accountMs),
  });
}

export function usePortfolio(accountId: string) {
  const { client, refresh } = useChronosServices();
  const streaming = useStreamStatus() === 'streaming';
  return useQuery({
    queryKey: queryKeys.portfolio(accountId),
    queryFn: ({ signal }) => client.getPortfolio(accountId, signal),
    refetchInterval: accountInterval(marketInterval(refresh, streaming)),
  });
}

// Newest first. Polling refetches every loaded page from the newest, deriving
// each later cursor from the fresh page before it, so a new order shifts the
// pages instead of leaving a gap between them.
export function useOrderHistory(accountId: string, instrument: string | null) {
  const { client, refresh } = useChronosServices();
  return useInfiniteQuery<OrderPage, Error, InfiniteData<OrderPage, string | null>, readonly unknown[], string | null>({
    queryKey: queryKeys.orders(accountId, instrument),
    queryFn: ({ pageParam, signal }) => client.listOrders(accountId, instrument, pageParam, signal),
    initialPageParam: null,
    getNextPageParam: (page) => page.next_cursor,
    refetchInterval: accountInterval(refresh.accountMs),
  });
}

export function useFillHistory(accountId: string, instrument: string | null) {
  const { client, refresh } = useChronosServices();
  return useInfiniteQuery<FillPage, Error, InfiniteData<FillPage, string | null>, readonly unknown[], string | null>({
    queryKey: queryKeys.fills(accountId, instrument),
    queryFn: ({ pageParam, signal }) => client.listFills(accountId, instrument, pageParam, signal),
    initialPageParam: null,
    getNextPageParam: (page) => page.next_cursor,
    refetchInterval: accountInterval(refresh.accountMs),
  });
}
