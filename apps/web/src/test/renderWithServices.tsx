import { QueryClientProvider } from '@tanstack/react-query';
import { render } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import type { ReactElement } from 'react';

import { SELECTED_ACCOUNT_KEY, RECENT_ACCOUNTS_KEY } from '../state/accountSelection';
import { createQueryClient } from '../state/queryClient';
import type { RefreshPolicy } from '../state/refreshPolicy';
import { ChronosServicesProvider } from '../state/services';
import type { FakeApi } from './fakeApi';
import { createFakeApi } from './fakeApi';
import type { Json } from './fixtures';
import { ACCOUNT_ID, accountJson, fillJson, healthJson, partialOrderJson, portfolioJson } from './fixtures';

// Tests trigger reads explicitly; the polling test opts back in.
export const NO_POLLING: RefreshPolicy = { healthMs: false, accountMs: false };

export function renderWithServices(
  ui: ReactElement,
  { api = createFakeApi(), refresh = NO_POLLING }: { api?: FakeApi; refresh?: RefreshPolicy } = {},
) {
  const queryClient = createQueryClient();
  const user = userEvent.setup();
  const view = render(
    <QueryClientProvider client={queryClient}>
      <ChronosServicesProvider services={{ client: api.client, refresh }}>{ui}</ChronosServicesProvider>
    </QueryClientProvider>,
  );
  return { ...view, api, queryClient, user };
}

/** Stores a selection as a previous visit would have left it. */
export function rememberSelectedAccount(accountId: string = ACCOUNT_ID) {
  window.localStorage.setItem(SELECTED_ACCOUNT_KEY, accountId);
  window.localStorage.setItem(RECENT_ACCOUNTS_KEY, JSON.stringify([accountId]));
}

export const accountPath = `/api/v1/accounts/${ACCOUNT_ID}`;

/** Serves a healthy account with one partially filled order and its fill. */
export function serveAccount(
  api: FakeApi,
  {
    account = accountJson(),
    portfolio = portfolioJson(),
    orders = [partialOrderJson()],
    fills = [fillJson()],
  }: { account?: Json; portfolio?: Json; orders?: Json[]; fills?: Json[] } = {},
) {
  api.on('GET', '/health', { status: 200, body: healthJson() });
  api.on('GET', accountPath, { status: 200, body: { account } });
  api.on('GET', `${accountPath}/portfolio`, { status: 200, body: { portfolio } });
  api.on('GET', `${accountPath}/orders`, { status: 200, body: { orders, next_cursor: null } });
  api.on('GET', `${accountPath}/fills`, { status: 200, body: { fills, next_cursor: null } });
}
