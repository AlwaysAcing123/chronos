import { QueryClient } from '@tanstack/react-query';

export function createQueryClient(): QueryClient {
  return new QueryClient({
    defaultOptions: {
      queries: {
        // Polling already repeats reads, and every failure view offers an
        // explicit retry, so automatic retries would only delay feedback.
        retry: false,
        // The API is on localhost; the browser's online flag says nothing
        // about whether it is reachable.
        networkMode: 'always',
        // Several components read the same resource; a second one mounting
        // within a second reuses the first read instead of repeating it.
        // Polling intervals and invalidation are unaffected.
        staleTime: 1_000,
        refetchOnWindowFocus: true,
      },
      mutations: { retry: false, networkMode: 'always' },
    },
  });
}
