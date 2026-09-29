import { act, screen, waitFor, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { App } from '../../App';
import { marketInterval } from '../../state/queries';
import { POLLING_REFRESH_POLICY } from '../../state/refreshPolicy';
import { createFakeApi } from '../../test/fakeApi';
import { fakeNoticeStreams } from '../../test/fakeNoticeStream';
import type { Json } from '../../test/fixtures';
import { errorJson, IEX_COVERAGE_NOTICE, liveSourceJson, marketDataJson, replaySourceJson } from '../../test/fixtures';
import {
  accountPath,
  rememberSelectedAccount,
  renderWithServices,
  serveAccount,
  serveServer,
} from '../../test/renderWithServices';

function renderSource(source: Json) {
  const api = createFakeApi();
  serveServer(api, marketDataJson({ source }));
  const rendered = renderWithServices(<App />, { api });
  return { ...rendered, api };
}

async function sourceBar() {
  return screen.findByRole('region', { name: 'Market-data source' });
}

describe('source bar', () => {
  it('labels a historical replay and its progress with exact server values', async () => {
    renderSource(replaySourceJson({ state: 'running', events_applied: '18446744073709551615' }));
    const bar = await sourceBar();
    expect(within(bar).getByText('Historical replay')).toBeInTheDocument();
    expect(within(bar).getByText('Databento EQUS.MINI MBP-1 replay')).toBeInTheDocument();
    expect(within(bar).getByText('Playing')).toBeInTheDocument();
    expect(within(bar).getByText('10x')).toBeInTheDocument();
    expect(within(bar).getByText('18446744073709551615')).toBeInTheDocument();
    expect(within(bar).getByText('2026-09-25 13:39:59.969574414')).toBeInTheDocument();
    expect(within(bar).getByRole('button', { name: 'Pause' })).toBeInTheDocument();
  });

  it('starts, pauses, and resumes the replay with empty commands', async () => {
    const { api, user } = renderSource(replaySourceJson({ state: 'ready' }));
    let state = 'ready';
    for (const [command, next] of [
      ['start', 'running'],
      ['pause', 'paused'],
      ['resume', 'running'],
    ] as const) {
      api.on('POST', `/api/v1/market-data/replay/${command}`, () => {
        state = next;
        return { status: 200, body: { market_data: marketDataJson({ source: replaySourceJson({ state }) }) } };
      });
    }
    const bar = await sourceBar();
    expect(within(bar).getByText('Ready to start')).toBeInTheDocument();
    await user.click(within(bar).getByRole('button', { name: 'Start replay' }));
    expect(await within(bar).findByText('Playing')).toBeInTheDocument();
    await user.click(within(bar).getByRole('button', { name: 'Pause' }));
    expect(await within(bar).findByText('Paused')).toBeInTheDocument();
    await user.click(within(bar).getByRole('button', { name: 'Resume' }));
    expect(await within(bar).findByText('Playing')).toBeInTheDocument();
    for (const command of ['start', 'pause', 'resume']) {
      expect(api.requestsTo('POST', `/api/v1/market-data/replay/${command}`)[0]?.body).toEqual({});
    }
  });

  it('explains a command the replay refused', async () => {
    const { api, user } = renderSource(replaySourceJson({ state: 'running' }));
    api.on('POST', '/api/v1/market-data/replay/pause', {
      status: 409,
      body: errorJson('invalid_replay_transition', 'cannot pause a replay that is completed', {
        reason: 'completed',
      }),
    });
    await user.click(within(await sourceBar()).getByRole('button', { name: 'Pause' }));
    expect(await screen.findByText('The replay cannot do that now')).toBeInTheDocument();
  });

  it('shows completion with the recording checksum and no controls', async () => {
    renderSource(replaySourceJson({ state: 'completed', recording_sha256: 'ab'.repeat(32) }));
    const bar = await sourceBar();
    expect(within(bar).getByText('Completed')).toBeInTheDocument();
    expect(within(bar).queryByRole('button')).not.toBeInTheDocument();
    expect(screen.getByText('Replay completed')).toBeInTheDocument();
    expect(screen.getByText('ab'.repeat(32))).toBeInTheDocument();
  });

  it('shows a failed source with its diagnostic', async () => {
    renderSource(replaySourceJson({ state: 'failed', diagnostic: 'recording rejected: record 7: truncated record' }));
    expect(await screen.findByText('Market-data source failed')).toBeInTheDocument();
    expect(screen.getByText('recording rejected: record 7: truncated record')).toBeInTheDocument();
  });

  it('offers no replay controls without a replay source', async () => {
    renderSource({
      mode: 'none',
      provider: null,
      feed: null,
      label: 'No market-data source',
      coverage_notice: null,
      coverage_label: null,
      state: 'none',
      replay_speed: null,
      events_applied: '0',
      diagnostic: null,
      recording_sha256: null,
      instruments: ['AAPL'],
    });
    const bar = await sourceBar();
    expect(within(bar).getByText('No market data')).toBeInTheDocument();
    expect(within(bar).queryByRole('button')).not.toBeInTheDocument();
  });
});

describe('live source bar', () => {
  function renderLive(source: Json, reasons?: Readonly<Record<string, string | null>>) {
    const api = createFakeApi();
    serveServer(api, marketDataJson({ source, ...(reasons ? { reasons } : {}) }));
    return { ...renderWithServices(<App />, { api }), api };
  }

  it('names Alpaca IEX as live and warns persistently that it is not SIP or NBBO', async () => {
    renderLive(liveSourceJson());
    const bar = await sourceBar();
    expect(within(bar).getByText('Live data')).toBeInTheDocument();
    expect(within(bar).getByText('Live')).toBeInTheDocument();
    expect(within(bar).getByText('Alpaca IEX')).toBeInTheDocument();
    expect(within(bar).getByText('IEX only, not SIP or NBBO')).toBeInTheDocument();
    expect(within(bar).getByText('42')).toBeInTheDocument();
    expect(within(bar).queryByRole('button')).not.toBeInTheDocument();
    expect(screen.getByText('Alpaca IEX is one exchange, not the whole market')).toBeInTheDocument();
    expect(screen.getByText(IEX_COVERAGE_NOTICE)).toBeInTheDocument();
    expect(screen.getByText(/can differ from the national best bid and offer/)).toBeInTheDocument();
    expect(screen.queryByText(/NBBO data\b/)).not.toBeInTheDocument();
  });

  it('shows the handshake state and that nothing is executable yet', async () => {
    renderLive(liveSourceJson({ state: 'authenticating', diagnostic: 'authenticating with Alpaca' }), {
      AAPL: 'source_unavailable',
      SPY: 'source_unavailable',
    });
    const bar = await sourceBar();
    expect(within(bar).getByText('Authenticating')).toBeInTheDocument();
    expect(screen.getByText('Connecting to Alpaca IEX')).toBeInTheDocument();
    expect(screen.getByText('authenticating with Alpaca')).toBeInTheDocument();
    expect(screen.getByText('No quote is executable until the feed is live.')).toBeInTheDocument();
    expect(screen.getByText('Alpaca IEX is one exchange, not the whole market')).toBeInTheDocument();
  });

  it('follows a disconnect and reconnect through change notices', async () => {
    const api = createFakeApi();
    let marketData = marketDataJson({ source: liveSourceJson() });
    api.on('GET', '/api/v1/market-data', () => ({ status: 200, body: { market_data: marketData } }));
    serveServer(api, marketData);
    api.on('GET', '/api/v1/market-data', () => ({ status: 200, body: { market_data: marketData } }));
    const notices = fakeNoticeStreams();
    renderWithServices(<App />, { api, openNoticeStream: notices.open });
    const bar = await sourceBar();
    expect(within(bar).getByText('Live')).toBeInTheDocument();
    act(() => {
      notices.streams[0]?.open();
    });

    marketData = marketDataJson({
      source: liveSourceJson({
        state: 'reconnecting',
        diagnostic: 'the connection closed: the WebSocket connection closed; reconnecting in 1.000 s',
      }),
      reasons: { AAPL: 'source_unavailable', SPY: 'source_unavailable' },
    });
    act(() => {
      notices.streams[0]?.notice('9');
    });
    expect(await screen.findByText('Live data interrupted')).toBeInTheDocument();
    expect(within(bar).getByText('Reconnecting')).toBeInTheDocument();
    expect(screen.getByText(/reconnecting in 1.000 s/)).toBeInTheDocument();
    expect(screen.getByText(/orders and valuation are unavailable until the feed reconnects/)).toBeInTheDocument();

    marketData = marketDataJson({
      source: liveSourceJson(),
      reasons: { AAPL: null, SPY: 'awaiting_fresh_quote' },
    });
    act(() => {
      notices.streams[0]?.notice('10');
    });
    expect(await screen.findByText('Waiting for fresh quotes')).toBeInTheDocument();
    expect(screen.getByText(/no current quote yet for SPY/)).toBeInTheDocument();
    expect(screen.queryByText('Live data interrupted')).not.toBeInTheDocument();

    marketData = marketDataJson({ source: liveSourceJson(), reasons: { AAPL: null, SPY: null } });
    act(() => {
      notices.streams[0]?.notice('11');
    });
    await waitFor(() => {
      expect(screen.queryByText('Waiting for fresh quotes')).not.toBeInTheDocument();
    });
    expect(screen.getByText('Alpaca IEX is one exchange, not the whole market')).toBeInTheDocument();
  });

  it('explains a failed live source without implying a usable quote', async () => {
    renderLive(
      liveSourceJson({
        state: 'failed',
        diagnostic: 'Alpaca error 402 (auth failed): the credentials were rejected; check APCA_API_KEY_ID',
      }),
      { AAPL: 'source_unavailable', SPY: 'source_unavailable' },
    );
    expect(await screen.findByText('Market-data source failed')).toBeInTheDocument();
    expect(screen.getByText(/Alpaca error 402/)).toBeInTheDocument();
    expect(screen.getByText('No quote is executable until the server restarts.')).toBeInTheDocument();
    expect(screen.queryByText(/stays frozen/)).not.toBeInTheDocument();
  });

  it('explains why an account has no usable live quote', async () => {
    rememberSelectedAccount();
    const api = createFakeApi();
    serveAccount(api);
    serveServer(
      api,
      marketDataJson({
        source: liveSourceJson({ state: 'reconnecting' }),
        reasons: { AAPL: 'source_unavailable', SPY: 'source_unavailable' },
      }),
    );
    api.on('GET', `${accountPath}/portfolio`, {
      status: 503,
      body: errorJson('market_data_unavailable', 'the live source is not connected', {
        reason: 'source_unavailable',
      }),
    });
    renderWithServices(<App />, { api });
    expect(await screen.findByText('No usable quote for this account')).toBeInTheDocument();
    expect(
      screen.getAllByText('The market-data source is unavailable, so no quote is current').length,
    ).toBeGreaterThan(0);
  });
});

describe('change stream', () => {
  afterEach(() => {
    vi.useRealTimers();
  });

  it('polls market data slowly only while the stream is open', () => {
    expect(marketInterval(POLLING_REFRESH_POLICY, true)).toBe(15_000);
    expect(marketInterval(POLLING_REFRESH_POLICY, false)).toBe(2_000);
  });

  it('refreshes market-dependent reads on each notice, not durable history', async () => {
    rememberSelectedAccount();
    const api = createFakeApi();
    serveAccount(api);
    const notices = fakeNoticeStreams();
    renderWithServices(<App />, { api, openNoticeStream: notices.open });
    await screen.findByText('99994.58');
    const stream = notices.streams[0];
    expect(stream).toBeDefined();
    act(() => {
      stream?.open();
    });
    expect(await screen.findByText(/^Streaming changes/)).toBeInTheDocument();

    const portfolioReads = () => api.requestsTo('GET', `${accountPath}/portfolio`).length;
    const marketReads = () => api.requestsTo('GET', '/api/v1/market-data').length;
    const orderReads = api.requestsTo('GET', `${accountPath}/orders`).length;
    const beforePortfolio = portfolioReads();
    const beforeMarket = marketReads();
    act(() => {
      stream?.notice('8');
    });
    await waitFor(() => {
      expect(portfolioReads()).toBeGreaterThan(beforePortfolio);
      expect(marketReads()).toBeGreaterThan(beforeMarket);
    });
    expect(api.requestsTo('GET', `${accountPath}/orders`)).toHaveLength(orderReads);
  });

  it('reads again after a notice that arrived while a read was in flight', async () => {
    const api = createFakeApi();
    serveServer(api, marketDataJson({ source: liveSourceJson() }));
    let release: (() => void) | undefined;
    let current = marketDataJson({ source: liveSourceJson() });
    api.on('GET', '/api/v1/market-data', () => {
      // The answer is taken when the request arrives, then held back.
      const answer = { status: 200, body: { market_data: current } };
      return new Promise((resolve) => {
        release = () => {
          resolve(answer);
        };
      });
    });
    const notices = fakeNoticeStreams();
    renderWithServices(<App />, { api, openNoticeStream: notices.open });
    await waitFor(() => {
      expect(release).toBeDefined();
    });
    act(() => {
      release?.();
    });
    const bar = await sourceBar();
    expect(within(bar).getByText('Live')).toBeInTheDocument();

    // Opening the stream starts a read; the source changes while it waits.
    act(() => {
      notices.streams[0]?.open();
    });
    const reads = api.requestsTo('GET', '/api/v1/market-data').length;
    current = marketDataJson({
      source: liveSourceJson({ state: 'reconnecting', diagnostic: 'the connection closed' }),
      reasons: { AAPL: 'source_unavailable', SPY: 'source_unavailable' },
    });
    act(() => {
      notices.streams[0]?.notice('12');
    });
    // The notice does not restart the in-flight read, which answers stale.
    expect(api.requestsTo('GET', '/api/v1/market-data')).toHaveLength(reads);
    const stale = release;
    act(() => {
      stale?.();
    });
    await waitFor(() => {
      expect(api.requestsTo('GET', '/api/v1/market-data')).toHaveLength(reads + 1);
    });
    act(() => {
      release?.();
    });
    expect(await within(bar).findByText('Reconnecting')).toBeInTheDocument();
  });

  it('falls back to polling and reconnects a refused stream with backoff', async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true });
    const api = createFakeApi();
    serveServer(api);
    const notices = fakeNoticeStreams();
    renderWithServices(<App />, { api, openNoticeStream: notices.open });
    expect(await screen.findByText(/^Connecting to change stream/)).toBeInTheDocument();

    act(() => {
      notices.streams[0]?.refuse();
    });
    expect(await screen.findByText(/^Not polling/)).toBeInTheDocument();
    expect(notices.streams[0]?.closed).toBe(true);
    expect(notices.streams).toHaveLength(1);
    await act(async () => {
      await vi.advanceTimersByTimeAsync(2_000);
    });
    expect(notices.streams).toHaveLength(2);
    act(() => {
      notices.streams[1]?.open();
    });
    expect(await screen.findByText(/^Streaming changes/)).toBeInTheDocument();

    // A dropped stream falls back to polling while the browser retries it.
    act(() => {
      notices.streams[1]?.drop();
    });
    expect(await screen.findByText(/^Not polling/)).toBeInTheDocument();
    expect(notices.streams).toHaveLength(2);
  });
});
