import type { ReactNode } from 'react';
import { createContext, useContext } from 'react';

import type { ChronosClient } from '../api/client';
import type { DemoClient } from '../api/demo';
import type { DeploymentClient } from '../api/deployments';
import type { ResearchClient } from '../api/research';
import type { OpenNoticeStream } from './marketDataStream';
import type { RefreshPolicy } from './refreshPolicy';

export interface ChronosServices {
  readonly client: ChronosClient;
  readonly refresh: RefreshPolicy;
  // Opens the server's change stream; without it the page only polls.
  readonly openNoticeStream?: OpenNoticeStream;
  // The local research service; without it Research says it is not configured.
  readonly research?: ResearchClient;
  // Model deployments on the trading server; without it Research shows none.
  readonly deployments?: DeploymentClient;
  // The demo workspace's guide data; only a demo build provides it.
  readonly demo?: DemoClient;
}

const ChronosServicesContext = createContext<ChronosServices | null>(null);

export function ChronosServicesProvider({
  services,
  children,
}: {
  readonly services: ChronosServices;
  readonly children: ReactNode;
}) {
  return <ChronosServicesContext value={services}>{children}</ChronosServicesContext>;
}

export function useChronosServices(): ChronosServices {
  const services = useContext(ChronosServicesContext);
  if (services === null) {
    throw new Error('useChronosServices requires a ChronosServicesProvider');
  }
  return services;
}
