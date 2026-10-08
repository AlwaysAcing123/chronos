// Comparison state. A comparison is immutable, so its detail is read once;
// the list refreshes when a comparison job succeeds. The job is polled with
// the model library's other jobs (useModelJob).
import { useInfiniteQuery, useMutation, useQuery } from '@tanstack/react-query';

import type { ComparisonPage, ComparisonRequest } from '../api/comparisonSchemas';
import { researchKeys, useResearchClient } from './research';

export const comparisonKeys = {
  all: [...researchKeys.all, 'comparisons'] as const,
  list: () => [...comparisonKeys.all, 'list'] as const,
  detail: (comparisonId: string) => [...comparisonKeys.all, 'detail', comparisonId] as const,
};

function client(research: ReturnType<typeof useResearchClient>) {
  if (research === null) {
    throw new Error('the research service is not configured');
  }
  return research;
}

export function useComparisons() {
  const research = useResearchClient();
  return useInfiniteQuery({
    queryKey: comparisonKeys.list(),
    queryFn: ({ pageParam, signal }) => client(research).listComparisons(pageParam, signal),
    initialPageParam: null as string | null,
    getNextPageParam: (page) => page.next_cursor,
    select: (data): ComparisonPage => ({
      comparisons: data.pages.flatMap((page) => page.comparisons),
      invalid_entries: data.pages.flatMap((page) => page.invalid_entries),
      next_cursor: data.pages.at(-1)?.next_cursor ?? null,
    }),
    enabled: research !== null,
  });
}

export function useComparison(comparisonId: string) {
  const research = useResearchClient();
  return useQuery({
    queryKey: comparisonKeys.detail(comparisonId),
    queryFn: ({ signal }) => client(research).getComparison(comparisonId, signal),
    enabled: research !== null,
  });
}

export function useStartComparison() {
  const research = useResearchClient();
  return useMutation({
    mutationFn: (request: ComparisonRequest) => client(research).startComparison(request),
  });
}
