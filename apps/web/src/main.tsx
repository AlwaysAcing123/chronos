import './styles/global.css';

import { QueryClientProvider } from '@tanstack/react-query';
import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import { createChronosClient } from './api/client';
import { App } from './App';
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
      <ChronosServicesProvider services={{ client: createChronosClient(), refresh: POLLING_REFRESH_POLICY }}>
        <App />
      </ChronosServicesProvider>
    </QueryClientProvider>
  </StrictMode>,
);
