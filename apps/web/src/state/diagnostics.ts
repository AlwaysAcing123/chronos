// Strategy-diagnostics state. Diagnostics are immutable, so each is read once
// per page load (a reload reads it, and its sources' checks, again); lists
// refresh when a diagnostics job succeeds, and a job is polled only while it
// is queued or running. Nothing here starts work except useStartDiagnostics.
import { useInfiniteQuery, useMutation, useQuery, useQueryClient } from '@tanstack/react-query';

import type { DiagnosticsPage, EpisodePage, FillPage, SimulatedAccount } from '../api/diagnosticsSchemas';
import { ACTIVE_JOB_POLL_MS, researchKeys, useResearchClient } from './research';

export const diagnosticsKeys = {
  all: [...researchKeys.all, 'strategy-diagnostics'] as const,
  list: (studyId: string | null) => [...diagnosticsKeys.all, 'list', studyId ?? 'all'] as const,
  detail: (diagnosticId: string) => [...diagnosticsKeys.all, 'detail', diagnosticId] as const,
  rows: (diagnosticId: string, kind: 'fills' | 'episodes', runId: string, account: SimulatedAccount) =>
    [...diagnosticsKeys.all, kind, diagnosticId, runId, account] as const,
  job: (jobId: string) => [...diagnosticsKeys.all, 'job', jobId] as const,
};

function client(research: ReturnType<typeof useResearchClient>) {
  if (research === null) {
    throw new Error('the research service is not configured');
  }
  return research;
}

export function useDiagnosticsList(studyId: string | null) {
  const research = useResearchClient();
  return useInfiniteQuery({
    queryKey: diagnosticsKeys.list(studyId),
    queryFn: ({ pageParam, signal }) => client(research).listDiagnostics(studyId, pageParam, signal),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.next_cursor,
    select: (data): DiagnosticsPage => ({
      diagnostics: data.pages.flatMap((page) => page.diagnostics),
      invalid_entries: data.pages.flatMap((page) => page.invalid_entries),
      next_cursor: data.pages.at(-1)?.next_cursor ?? null,
    }),
    enabled: research !== null,
  });
}

export function useDiagnostics(diagnosticId: string) {
  const research = useResearchClient();
  return useQuery({
    queryKey: diagnosticsKeys.detail(diagnosticId),
    queryFn: ({ signal }) => client(research).getDiagnostics(diagnosticId, signal),
    enabled: research !== null,
    staleTime: Infinity,
  });
}

export function useDiagnosticFills(diagnosticId: string, runId: string, account: SimulatedAccount) {
  const research = useResearchClient();
  return useInfiniteQuery({
    queryKey: diagnosticsKeys.rows(diagnosticId, 'fills', runId, account),
    queryFn: ({ pageParam, signal }) =>
      client(research).listDiagnosticFills(diagnosticId, runId, account, pageParam, signal),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.next_cursor,
    select: (data): FillPage => ({
      fills: data.pages.flatMap((page) => page.fills),
      next_cursor: data.pages.at(-1)?.next_cursor ?? null,
    }),
    enabled: research !== null,
    staleTime: Infinity,
  });
}

export function useDiagnosticEpisodes(diagnosticId: string, runId: string, account: SimulatedAccount) {
  const research = useResearchClient();
  return useInfiniteQuery({
    queryKey: diagnosticsKeys.rows(diagnosticId, 'episodes', runId, account),
    queryFn: ({ pageParam, signal }) =>
      client(research).listDiagnosticEpisodes(diagnosticId, runId, account, pageParam, signal),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.next_cursor,
    select: (data): EpisodePage => ({
      episodes: data.pages.flatMap((page) => page.episodes),
      next_cursor: data.pages.at(-1)?.next_cursor ?? null,
    }),
    enabled: research !== null,
    staleTime: Infinity,
  });
}

export function useDiagnosticsJob(jobId: string | null) {
  const research = useResearchClient();
  const queryClient = useQueryClient();
  return useQuery({
    queryKey: diagnosticsKeys.job(jobId ?? 'none'),
    queryFn: async ({ signal }) => {
      const job = await client(research).getDiagnosticsJob(jobId ?? '', signal);
      if (job.state === 'succeeded') {
        void queryClient.invalidateQueries({ queryKey: [...diagnosticsKeys.all, 'list'] });
      }
      return job;
    },
    enabled: research !== null && jobId !== null,
    refetchInterval: (query) => {
      const state = query.state.data?.state;
      return state === 'queued' || state === 'running' ? ACTIVE_JOB_POLL_MS : false;
    },
  });
}

export function useStartDiagnostics() {
  const research = useResearchClient();
  return useMutation({
    mutationFn: (studyIds: readonly string[]) => client(research).startDiagnostics(studyIds),
  });
}
