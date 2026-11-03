import './styles/global.css';

import { QueryClientProvider } from '@tanstack/react-query';
import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import { createChronosClient } from './api/client';
import { createDemoClient } from './api/demo';
import { createDeploymentClient } from './api/deployments';
import { createResearchClient } from './api/research';
import { App } from './App';
import { DEMO_ENABLED } from './state/demo';
import { createQueryClient } from './state/queryClient';
import { POLLING_REFRESH_POLICY } from './state/refreshPolicy';
import { ChronosServicesProvider } from './state/services';

const root = document.getElementById('root');
if (root === null) {
  throw new Error('index.html must contain #root');
}

createRoot(root).render(
  <StrictMode>
    <QueryClientProvider client={createQueryClient()}>
      <ChronosServicesProvider
        services={{
          client: createChronosClient(),
          research: createResearchClient(),
          deployments: createDeploymentClient(),
          refresh: POLLING_REFRESH_POLICY,
          openNoticeStream: () => new EventSource('/api/v1/market-data/events'),
          ...(DEMO_ENABLED ? { demo: createDemoClient() } : {}),
        }}
      >
        <App />
      </ChronosServicesProvider>
    </QueryClientProvider>
  </StrictMode>,
);
