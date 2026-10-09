// Walk-forward state. Plans and studies are immutable, so each is read once;
// the lists refresh when a job succeeds. Jobs are polled only while queued
// or running.
import { useInfiniteQuery, useMutation, useQuery, useQueryClient } from '@tanstack/react-query';

import type { CollectionPage, StudyJobPage, StudyPage, WalkForwardRequest } from '../api/walkForwardSchemas';
import { ACTIVE_JOB_POLL_MS, researchKeys, useResearchClient } from './research';

export const walkForwardKeys = {
  all: [...researchKeys.all, 'walk-forward'] as const,
  collections: () => [...walkForwardKeys.all, 'collections'] as const,
  plan: (planId: string) => [...walkForwardKeys.all, 'plan', planId] as const,
  studies: () => [...walkForwardKeys.all, 'studies'] as const,
  study: (studyId: string) => [...walkForwardKeys.all, 'study', studyId] as const,
  job: (jobId: string) => [...walkForwardKeys.all, 'job', jobId] as const,
  jobs: () => [...walkForwardKeys.all, 'jobs'] as const,
};

function client(research: ReturnType<typeof useResearchClient>) {
  if (research === null) {
    throw new Error('the research service is not configured');
  }
  return research;
}

export function useCollections() {
  const research = useResearchClient();
  return useInfiniteQuery({
    queryKey: walkForwardKeys.collections(),
    queryFn: ({ pageParam, signal }) => client(research).listCollections(pageParam, signal),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.next_cursor,
    select: (data): CollectionPage => ({
      collections: data.pages.flatMap((page) => page.collections),
      invalid_entries: data.pages.flatMap((page) => page.invalid_entries),
      next_cursor: data.pages.at(-1)?.next_cursor ?? null,
    }),
    enabled: research !== null,
  });
}

export function usePlan(planId: string) {
  const research = useResearchClient();
  return useQuery({
    queryKey: walkForwardKeys.plan(planId),
    queryFn: ({ signal }) => client(research).getWalkForwardPlan(planId, signal),
    enabled: research !== null,
    staleTime: Infinity,
  });
}

export function useStudies() {
  const research = useResearchClient();
  return useInfiniteQuery({
    queryKey: walkForwardKeys.studies(),
    queryFn: ({ pageParam, signal }) => client(research).listWalkForwardStudies(pageParam, signal),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.next_cursor,
    select: (data): StudyPage => ({
      studies: data.pages.flatMap((page) => page.studies),
      invalid_entries: data.pages.flatMap((page) => page.invalid_entries),
      next_cursor: data.pages.at(-1)?.next_cursor ?? null,
    }),
    enabled: research !== null,
  });
}

export function useStudy(studyId: string) {
  const research = useResearchClient();
  return useQuery({
    queryKey: walkForwardKeys.study(studyId),
    queryFn: ({ signal }) => client(research).getWalkForwardStudy(studyId, signal),
    enabled: research !== null,
    staleTime: Infinity,
  });
}

export function useWalkForwardJob(jobId: string | null) {
  const research = useResearchClient();
  const queryClient = useQueryClient();
  return useQuery({
    queryKey: walkForwardKeys.job(jobId ?? 'none'),
    queryFn: async ({ signal }) => {
      const job = await client(research).getWalkForwardJob(jobId ?? '', signal);
      if (job.state === 'succeeded' && job.kind === 'walk_forward') {
        void queryClient.invalidateQueries({ queryKey: walkForwardKeys.studies() });
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

export function hasActiveStudyJob(page: StudyJobPage | undefined): boolean {
  return page?.jobs.some((job) => job.state === 'queued' || job.state === 'running') ?? false;
}

export function useStudyJobs() {
  const research = useResearchClient();
  const queryClient = useQueryClient();
  return useInfiniteQuery({
    queryKey: walkForwardKeys.jobs(),
    queryFn: async ({ pageParam, signal }) => {
      const page = await client(research).listStudyJobs(pageParam, signal);
      if (page.jobs.some((job) => job.state === 'succeeded')) {
        void queryClient.invalidateQueries({ queryKey: walkForwardKeys.studies() });
      }
      return page;
    },
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.next_cursor,
    select: (data): StudyJobPage => ({
      jobs: data.pages.flatMap((page) => page.jobs),
      next_cursor: data.pages.at(-1)?.next_cursor ?? null,
    }),
    enabled: research !== null,
    refetchInterval: (query) =>
      query.state.data?.pages.some((page) => hasActiveStudyJob(page)) === true ? ACTIVE_JOB_POLL_MS : false,
  });
}

export function usePreviewWalkForward() {
  const research = useResearchClient();
  return useMutation({
    mutationFn: (request: WalkForwardRequest) => client(research).previewWalkForward(request),
  });
}

export function useStartStudy() {
  const research = useResearchClient();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (planId: string) => client(research).startWalkForwardStudy(planId),
    onSettled: () => queryClient.invalidateQueries({ queryKey: walkForwardKeys.jobs() }),
  });
}
