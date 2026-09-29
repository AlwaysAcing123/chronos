import type { ReactNode } from 'react';
import { createContext, useContext } from 'react';

import type { ChronosClient } from '../api/client';
import type { OpenNoticeStream } from './marketDataStream';
import type { RefreshPolicy } from './refreshPolicy';

export interface ChronosServices {
  readonly client: ChronosClient;
  readonly refresh: RefreshPolicy;
  // Opens the server's change stream; without it the page only polls.
  readonly openNoticeStream?: OpenNoticeStream;
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
