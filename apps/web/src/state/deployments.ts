// Model deployment state. Deployments are read from the trading server; the
// list polls quickly while one is running or stopping, slowly otherwise, and a control
// refreshes both the deployments and the account it trades.
import type { InfiniteData } from '@tanstack/react-query';
import { useInfiniteQuery, useMutation, useQuery, useQueryClient } from '@tanstack/react-query';

import type { DeploymentClient, DeploymentPage, DeploymentRequest } from '../api/deployments';
import { queryKeys } from './queries';
import { useChronosServices } from './services';

export const deploymentKeys = {
  all: ['chronos-deployments'] as const,
  list: () => [...deploymentKeys.all, 'list'] as const,
  decisions: (deploymentId: string) => [...deploymentKeys.all, 'decisions', deploymentId] as const,
};

// While a deployment is running, its state and counters are re-read this
// often; idle polling also discovers a control that completes after its
// response times out, or a control made in another browser.
export const ACTIVE_DEPLOYMENT_POLL_MS = 1_000;
export const IDLE_DEPLOYMENT_POLL_MS = 5_000;

export function useDeploymentClient(): DeploymentClient | null {
  return useChronosServices().deployments ?? null;
}

function required(client: DeploymentClient | null): DeploymentClient {
  if (client === null) {
    throw new Error('model deployments are not configured');
  }
  return client;
}

export function hasActiveDeployment(page: DeploymentPage | undefined): boolean {
  return page?.deployments.some((deployment) => deployment.state === 'running' || deployment.state === 'stopping') ?? false;
}

export function useDeployments() {
  const client = useDeploymentClient();
  return useInfiniteQuery({
    queryKey: deploymentKeys.list(),
    queryFn: ({ pageParam, signal }) => required(client).listDeployments(pageParam, signal),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.next_cursor,
    enabled: client !== null,
    select: (data: InfiniteData<DeploymentPage>): DeploymentPage => ({
      enabled: data.pages[0]?.enabled ?? false,
      deployments: data.pages.flatMap((page) => page.deployments),
      next_cursor: data.pages.at(-1)?.next_cursor ?? null,
    }),
    refetchInterval: (query) =>
      query.state.data?.pages.some((page) => hasActiveDeployment(page)) === true
        ? ACTIVE_DEPLOYMENT_POLL_MS : IDLE_DEPLOYMENT_POLL_MS,
  });
}

export function useDecisions(deploymentId: string, active: boolean) {
  const client = useDeploymentClient();
  return useQuery({
    queryKey: deploymentKeys.decisions(deploymentId),
    queryFn: ({ signal }) => required(client).listDecisions(deploymentId, null, signal),
    enabled: client !== null,
    refetchInterval: active ? ACTIVE_DEPLOYMENT_POLL_MS : false,
  });
}

function useRefreshAfterControl() {
  const queryClient = useQueryClient();
  return (accountIds: readonly string[]) =>
    Promise.all([
      queryClient.invalidateQueries({ queryKey: deploymentKeys.all }),
      ...accountIds.map((accountId) => queryClient.invalidateQueries({ queryKey: queryKeys.account(accountId) })),
    ]);
}

export function useCreateDeployment() {
  const client = useDeploymentClient();
  const refresh = useRefreshAfterControl();
  return useMutation({
    mutationFn: (request: DeploymentRequest) => required(client).createDeployment(request),
    onSettled: (_data, _error, request) => refresh([request.account_id]),
  });
}

export function useStartDeployment() {
  const client = useDeploymentClient();
  const refresh = useRefreshAfterControl();
  return useMutation({
    mutationFn: (deployment: { readonly id: string; readonly accountId: string }) =>
      required(client).startDeployment(deployment.id),
    onSettled: (_data, _error, deployment) => refresh([deployment.accountId]),
  });
}

export function useStopDeployment() {
  const client = useDeploymentClient();
  const refresh = useRefreshAfterControl();
  return useMutation({
    mutationFn: (deployment: { readonly id: string; readonly accountId: string }) =>
      required(client).stopDeployment(deployment.id),
    onSettled: (_data, _error, deployment) => refresh([deployment.accountId]),
  });
}

export function useKillSwitch() {
  const client = useDeploymentClient();
  const refresh = useRefreshAfterControl();
  return useMutation({
    mutationFn: () => required(client).killSwitch(),
    onSettled: (stopped) => refresh(stopped?.map((deployment) => deployment.account_id) ?? []),
  });
}
