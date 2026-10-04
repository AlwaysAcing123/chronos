// Research service state. Components read the service only through these
// hooks. Published results are immutable, so they are read once; the job
// list polls only while a job is queued or running.
import { useInfiniteQuery, useMutation, useQuery, useQueryClient } from '@tanstack/react-query';

import type { ResearchClient } from '../api/research';
import type { AccountName, JobPage, RunRequest } from '../api/researchSchemas';
import { useChronosServices } from './services';

export const researchKeys = {
  all: ['chronos-research'] as const,
  status: () => [...researchKeys.all, 'status'] as const,
  experiments: () => [...researchKeys.all, 'experiments'] as const,
  runs: () => [...researchKeys.all, 'runs'] as const,
  results: () => [...researchKeys.all, 'results'] as const,
  result: (runId: string) => [...researchKeys.all, 'results', runId] as const,
  trades: (runId: string, account: AccountName) => [...researchKeys.result(runId), 'trades', account] as const,
  decisions: (runId: string) => [...researchKeys.result(runId), 'decisions'] as const,
  equity: (runId: string) => [...researchKeys.result(runId), 'equity'] as const,
};

// While a job is queued or running its status is re-read this often.
export const ACTIVE_JOB_POLL_MS = 1_000;

export function useResearchClient(): ResearchClient | null {
  return useChronosServices().research ?? null;
}

function required(client: ResearchClient | null): ResearchClient {
  if (client === null) {
    throw new Error('the research service is not configured');
  }
  return client;
}

export function useResearchStatus() {
  const client = useResearchClient();
  return useQuery({
    queryKey: researchKeys.status(),
    queryFn: ({ signal }) => required(client).getStatus(signal),
    enabled: client !== null,
  });
}

export function useExperiments() {
  const client = useResearchClient();
  return useQuery({
    queryKey: researchKeys.experiments(),
    queryFn: ({ signal }) => required(client).listExperiments(signal),
    enabled: client !== null,
  });
}

export function hasActiveJob(page: JobPage | undefined): boolean {
  return page?.runs.some((job) => job.state === 'queued' || job.state === 'running') ?? false;
}

export function useRuns() {
  const client = useResearchClient();
  const queryClient = useQueryClient();
  return useQuery({
    queryKey: researchKeys.runs(),
    queryFn: async ({ signal }) => {
      const page = await required(client).listRuns(signal);
      // A job that finished may have published a result.
      if (page.runs.some((job) => job.state === 'succeeded')) {
        void queryClient.invalidateQueries({ queryKey: researchKeys.results(), exact: true });
      }
      return page;
    },
    enabled: client !== null,
    refetchInterval: (query) => (hasActiveJob(query.state.data) ? ACTIVE_JOB_POLL_MS : false),
  });
}

export function useStartRun() {
  const client = useResearchClient();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (request: RunRequest) => required(client).startRun(request),
    onSettled: () => queryClient.invalidateQueries({ queryKey: researchKeys.runs() }),
  });
}

export function useResults() {
  const client = useResearchClient();
  return useQuery({
    queryKey: researchKeys.results(),
    queryFn: ({ signal }) => required(client).listResults(signal),
    enabled: client !== null,
  });
}

export function useResult(runId: string) {
  const client = useResearchClient();
  return useQuery({
    queryKey: researchKeys.result(runId),
    queryFn: ({ signal }) => required(client).getResult(runId, signal),
    enabled: client !== null,
    staleTime: Infinity,
  });
}

export function useEquity(runId: string) {
  const client = useResearchClient();
  return useQuery({
    queryKey: researchKeys.equity(runId),
    queryFn: ({ signal }) => required(client).getEquity(runId, signal),
    enabled: client !== null,
    staleTime: Infinity,
  });
}

export function useTrades(runId: string, account: AccountName) {
  const client = useResearchClient();
  return useInfiniteQuery({
    queryKey: researchKeys.trades(runId, account),
    queryFn: ({ pageParam, signal }) => required(client).listTrades(runId, account, pageParam, signal),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.next_cursor,
    enabled: client !== null,
    staleTime: Infinity,
  });
}

export function useDecisions(runId: string) {
  const client = useResearchClient();
  return useInfiniteQuery({
    queryKey: researchKeys.decisions(runId),
    queryFn: ({ pageParam, signal }) => required(client).listDecisions(runId, pageParam, signal),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.next_cursor,
    enabled: client !== null,
    staleTime: Infinity,
  });
}
