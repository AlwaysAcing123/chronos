// The demo workspace's guide (ADR 0025). It exists only in a build made with
// VITE_CHRONOS_DEMO=1, which the demo supervisor alone sets; an ordinary
// build has no guide route, no Guide link, and never asks for its data.
import { useQuery } from '@tanstack/react-query';

import { useChronosServices } from './services';

export const DEMO_ENABLED = import.meta.env.VITE_CHRONOS_DEMO === '1';

export const demoKeys = {
  guide: () => ['chronos-demo', 'guide'] as const,
};

export function useDemoGuide() {
  const client = useChronosServices().demo ?? null;
  return useQuery({
    queryKey: demoKeys.guide(),
    queryFn: ({ signal }) => {
      if (client === null) {
        throw new Error('the demo guide is not configured');
      }
      return client.getGuide(signal);
    },
    enabled: client !== null,
  });
}
