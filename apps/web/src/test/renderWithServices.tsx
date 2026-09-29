import { QueryClientProvider } from '@tanstack/react-query';
import { render } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import type { ReactElement } from 'react';

import { SELECTED_ACCOUNT_KEY, RECENT_ACCOUNTS_KEY } from '../state/accountSelection';
import type { OpenNoticeStream } from '../state/marketDataStream';
import { createQueryClient } from '../state/queryClient';
import type { RefreshPolicy } from '../state/refreshPolicy';
import { ChronosServicesProvider } from '../state/services';
import type { FakeApi } from './fakeApi';
import { createFakeApi } from './fakeApi';
import type { Json } from './fixtures';
import { ACCOUNT_ID, accountJson, fillJson, healthJson, marketDataJson, partialOrderJson, portfolioJson } from './fixtures';

// Tests trigger reads explicitly; the polling test opts back in.
export const NO_POLLING: RefreshPolicy = {
  healthMs: false,
  accountMs: false,
  marketMs: false,
  streamedMarketMs: false,
};

export function renderWithServices(
  ui: ReactElement,
  {
    api = createFakeApi(),
    refresh = NO_POLLING,
    openNoticeStream,
  }: { api?: FakeApi; refresh?: RefreshPolicy; openNoticeStream?: OpenNoticeStream } = {},
) {
  const queryClient = createQueryClient();
  const user = userEvent.setup();
  const services = openNoticeStream ? { client: api.client, refresh, openNoticeStream } : { client: api.client, refresh };
  const view = render(
    <QueryClientProvider client={queryClient}>
      <ChronosServicesProvider services={services}>{ui}</ChronosServicesProvider>
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

/** Serves health and market data for a running replay. */
export function serveServer(api: FakeApi, marketData: Json = marketDataJson()) {
  api.on('GET', '/health', { status: 200, body: healthJson() });
  api.on('GET', '/api/v1/market-data', { status: 200, body: { market_data: marketData } });
}

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
  serveServer(api);
  api.on('GET', accountPath, { status: 200, body: { account } });
  api.on('GET', `${accountPath}/portfolio`, { status: 200, body: { portfolio } });
  api.on('GET', `${accountPath}/orders`, { status: 200, body: { orders, next_cursor: null } });
  api.on('GET', `${accountPath}/fills`, { status: 200, body: { fills, next_cursor: null } });
}
