// Model library state. A model version is immutable, so its detail is read
// once and only its lifecycle is refreshed after a change. A registration or
// import job is polled only while it is queued or running.
import { useInfiniteQuery, useMutation, useQuery, useQueryClient } from '@tanstack/react-query';

import type { LifecycleState, ModelPage, RegistrationRequest } from '../api/modelSchemas';
import { ACTIVE_JOB_POLL_MS, researchKeys, useResearchClient } from './research';

export type LibraryFilter = LifecycleState | 'all';

export const modelKeys = {
  all: [...researchKeys.all, 'models'] as const,
  list: (filter: LibraryFilter) => [...modelKeys.all, 'list', filter] as const,
  detail: (modelVersionId: string) => [...modelKeys.all, 'detail', modelVersionId] as const,
  job: (jobId: string) => [...modelKeys.all, 'job', jobId] as const,
};

function client(research: ReturnType<typeof useResearchClient>) {
  if (research === null) {
    throw new Error('the research service is not configured');
  }
  return research;
}

export function useModels(filter: LibraryFilter) {
  const research = useResearchClient();
  return useInfiniteQuery({
    queryKey: modelKeys.list(filter),
    queryFn: ({ pageParam, signal }) => client(research).listModels(filter, pageParam, signal),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.next_cursor,
    select: (data): ModelPage => ({
      models: data.pages.flatMap((page) => page.models),
      invalid_entries: data.pages.flatMap((page) => page.invalid_entries),
      next_cursor: data.pages.at(-1)?.next_cursor ?? null,
    }),
    enabled: research !== null,
  });
}

export function useModel(modelVersionId: string) {
  const research = useResearchClient();
  return useQuery({
    queryKey: modelKeys.detail(modelVersionId),
    queryFn: ({ signal }) => client(research).getModel(modelVersionId, signal),
    enabled: research !== null,
  });
}

export function useStartRegistration() {
  const research = useResearchClient();
  return useMutation({
    mutationFn: (request: RegistrationRequest) => client(research).startRegistration(request),
  });
}

// A registration or import job, polled while it is queued or running.
export function useModelJob(jobId: string | null) {
  const research = useResearchClient();
  const queryClient = useQueryClient();
  return useQuery({
    queryKey: modelKeys.job(jobId ?? 'none'),
    queryFn: async ({ signal }) => {
      const job = await client(research).getModelJob(jobId ?? '', signal);
      if (job.state === 'succeeded') {
        void queryClient.invalidateQueries({ queryKey: [...modelKeys.all, 'list'] });
        void queryClient.invalidateQueries({ queryKey: [...researchKeys.all, 'comparisons', 'list'] });
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

export type ImportStep = 'stage' | 'upload' | 'validate';

export class ImportStepError extends Error {
  readonly step: ImportStep;

  constructor(step: ImportStep, cause: unknown) {
    super(`the import failed at its ${step} step`, { cause });
    this.step = step;
  }
}

export interface ReviewedImport {
  readonly manifestText: string;
  readonly model: Uint8Array<ArrayBuffer>;
  // Computed in the browser from the bytes reviewed.
  readonly modelSha256: string;
}

// Stages the manifest, uploads the model, and starts validation of exactly
// the reviewed bytes. A step that fails is named; nothing later is sent.
export function useImportModel() {
  const research = useResearchClient();
  return useMutation({
    mutationFn: async (reviewed: ReviewedImport) => {
      const api = client(research);
      const step = async <Result>(name: ImportStep, run: () => Promise<Result>): Promise<Result> => {
        try {
          return await run();
        } catch (error) {
          throw new ImportStepError(name, error);
        }
      };
      const staged = await step('stage', () => api.stageImport(reviewed.manifestText));
      const uploaded = await step('upload', () => api.uploadImportModel(staged.import.import_id, reviewed.model));
      if (uploaded.import.model?.sha256 !== reviewed.modelSha256) {
        throw new ImportStepError('upload', new Error('the service received different bytes than were reviewed'));
      }
      return step('validate', () => api.startImportValidation(staged.import.import_id, reviewed.modelSha256));
    },
  });
}

export function useSetModelLifecycle(modelVersionId: string) {
  const research = useResearchClient();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: ({ state, reason }: { readonly state: LifecycleState; readonly reason: string }) =>
      client(research).setModelLifecycle(modelVersionId, state, reason),
    onSettled: () => queryClient.invalidateQueries({ queryKey: modelKeys.all }),
  });
}
