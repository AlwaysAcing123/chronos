// Server state hooks. Components read resources only through these hooks, so
// replacing polling with a live subscription changes this file, not them.
import type { InfiniteData } from '@tanstack/react-query';
import { useInfiniteQuery, useQuery } from '@tanstack/react-query';

import type { FillPage, OrderPage } from '../api/schemas';
import { isAccountNotFound } from '../api/errors';
import type { RefreshPolicy } from './refreshPolicy';
import { useChronosServices } from './services';

export const queryKeys = {
  all: ['chronos'] as const,
  health: () => [...queryKeys.all, 'health'] as const,
  account: (accountId: string) => [...queryKeys.all, 'accounts', accountId] as const,
  accountResource: (accountId: string) => [...queryKeys.account(accountId), 'resource'] as const,
  portfolio: (accountId: string) => [...queryKeys.account(accountId), 'portfolio'] as const,
  orders: (accountId: string) => [...queryKeys.account(accountId), 'orders'] as const,
  fills: (accountId: string) => [...queryKeys.account(accountId), 'fills'] as const,
};

// Stops polling an account the server says does not exist.
function accountInterval(refresh: RefreshPolicy) {
  return (query: { readonly state: { readonly error: unknown } }) =>
    isAccountNotFound(query.state.error) ? false : refresh.accountMs;
}

export function useHealth() {
  const { client, refresh } = useChronosServices();
  return useQuery({
    queryKey: queryKeys.health(),
    queryFn: ({ signal }) => client.getHealth(signal),
    refetchInterval: refresh.healthMs,
  });
}

export function useAccount(accountId: string) {
  const { client, refresh } = useChronosServices();
  return useQuery({
    queryKey: queryKeys.accountResource(accountId),
    queryFn: ({ signal }) => client.getAccount(accountId, signal),
    refetchInterval: accountInterval(refresh),
  });
}

export function usePortfolio(accountId: string) {
  const { client, refresh } = useChronosServices();
  return useQuery({
    queryKey: queryKeys.portfolio(accountId),
    queryFn: ({ signal }) => client.getPortfolio(accountId, signal),
    refetchInterval: accountInterval(refresh),
  });
}

// Newest first. Polling refetches every loaded page from the newest, deriving
// each later cursor from the fresh page before it, so a new order shifts the
// pages instead of leaving a gap between them.
export function useOrderHistory(accountId: string) {
  const { client, refresh } = useChronosServices();
  return useInfiniteQuery<OrderPage, Error, InfiniteData<OrderPage, string | null>, readonly unknown[], string | null>({
    queryKey: queryKeys.orders(accountId),
    queryFn: ({ pageParam, signal }) => client.listOrders(accountId, pageParam, signal),
    initialPageParam: null,
    getNextPageParam: (page) => page.next_cursor,
    refetchInterval: accountInterval(refresh),
  });
}

export function useFillHistory(accountId: string) {
  const { client, refresh } = useChronosServices();
  return useInfiniteQuery<FillPage, Error, InfiniteData<FillPage, string | null>, readonly unknown[], string | null>({
    queryKey: queryKeys.fills(accountId),
    queryFn: ({ pageParam, signal }) => client.listFills(accountId, pageParam, signal),
    initialPageParam: null,
    getNextPageParam: (page) => page.next_cursor,
    refetchInterval: accountInterval(refresh),
  });
}
