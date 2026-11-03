import { useMutation, useQueryClient } from '@tanstack/react-query';
import type { ReactNode } from 'react';

import type { ReplayCommand } from '../../api/client';
import { categoryOf } from '../../api/errors';
import type { MarketData, MarketDataSourceState } from '../../api/schemas';
import { FAILURE, MARKET_DATA_MODE, SOURCE_STATE } from '../../presentation/vocabulary';
import { queryKeys, useMarketData } from '../../state/queries';
import { useChronosServices } from '../../state/services';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { MarketTime } from '../ui/MarketTime';
import styles from './source.module.css';

// The replay command each state offers; live and finished sources offer none.
const REPLAY_ACTION: Partial<Record<MarketDataSourceState, { command: ReplayCommand; label: string }>> = {
  ready: { command: 'start', label: 'Start replay' },
  running: { command: 'pause', label: 'Pause' },
  paused: { command: 'resume', label: 'Resume' },
};

function ReplayControl({ state }: { readonly state: MarketDataSourceState }) {
  const { client } = useChronosServices();
  const queryClient = useQueryClient();
  const command = useMutation({
    mutationFn: (replay: ReplayCommand) => client.controlReplay(replay),
    onSuccess: (marketData) => {
      queryClient.setQueryData(queryKeys.marketData(), marketData);
    },
  });
  const action = REPLAY_ACTION[state];
  const failure = command.isError ? categoryOf(command.error) : null;
  return (
    <>
      {action && (
        <div className={styles.controls}>
          <Button
            variant="primary"
            disabled={command.isPending}
            onClick={() => {
              command.mutate(action.command);
            }}
          >
            {command.isPending ? 'Sending…' : action.label}
          </Button>
        </div>
      )}
      {failure !== null && (
        <div className={styles.notices}>
          <Alert tone="caution" title={FAILURE[failure].title} live>
            {FAILURE[failure].detail}
          </Alert>
        </div>
      )}
    </>
  );
}

// Live states before the first subscription completes.
const LIVE_HANDSHAKE: ReadonlySet<MarketDataSourceState> = new Set(['connecting', 'authenticating', 'subscribing']);

// Instruments a live feed has not yet delivered a current quote for.
function awaitingQuotes(marketData: MarketData): string[] {
  return marketData.instruments
    .filter(
      (item) => item.unavailable_reason === 'awaiting_fresh_quote' || item.unavailable_reason === 'no_top_of_book',
    )
    .map((item) => item.instrument);
}

// What the source's data is, in the source's own words: invented prices
// must never read as a market, a recording as more than its window, and a
// single-venue feed as the whole market.
function coverageTitle(source: MarketData['source']): string {
  if (source.synthetic) {
    return 'Synthetic data: invented prices, not market data';
  }
  return source.mode === 'live' ? `${source.label} is one exchange, not the whole market` : 'What this recording covers';
}

function CoverageNotice({ source }: { readonly source: MarketData['source'] }) {
  if (source.coverage_notice === null) {
    return null;
  }
  return (
    <Alert tone="caution" title={coverageTitle(source)}>
      <p>{source.coverage_notice}</p>
      {source.mode === 'live' && (
        <p>
          Quotes, sizes, and simulated fills use this exchange alone and can differ from the national best bid and
          offer.
        </p>
      )}
    </Alert>
  );
}

function LiveNotice({ marketData }: { readonly marketData: MarketData }) {
  const { source } = marketData;
  if (LIVE_HANDSHAKE.has(source.state)) {
    return (
      <Alert tone="neutral" title={`Connecting to ${source.label}`}>
        {source.diagnostic !== null && <p>{source.diagnostic}</p>}
        <p>No quote is executable until the feed is live.</p>
      </Alert>
    );
  }
  if (source.state === 'reconnecting') {
    return (
      <Alert tone="caution" title="Live data interrupted" live>
        {source.diagnostic !== null && <p>{source.diagnostic}</p>}
        <p>
          The last quotes are not current, so orders and valuation are unavailable until the feed reconnects and a
          new quote arrives for each instrument.
        </p>
      </Alert>
    );
  }
  if (source.state === 'live') {
    const waiting = awaitingQuotes(marketData);
    if (waiting.length > 0) {
      return (
        <Alert tone="neutral" title="Waiting for fresh quotes">
          <p>
            Connected, but no current quote yet for {waiting.join(', ')}. Those instruments cannot trade until one
            arrives.
          </p>
        </Alert>
      );
    }
  }
  return null;
}

function SourceNotices({ marketData }: { readonly marketData: MarketData }) {
  const { source } = marketData;
  let status: ReactNode = null;
  if (source.state === 'failed') {
    status = (
      <Alert tone="negative" title="Market-data source failed" live>
        <p>{source.diagnostic ?? 'The source stopped without a diagnostic.'}</p>
        <p>No quote is executable until the server restarts.</p>
      </Alert>
    );
  } else if (source.state === 'completed') {
    status = (
      <Alert tone="neutral" title="Replay completed">
        <p>
          All {source.events_applied} events were played. The market stays at the recording’s final state; restart
          the server, and use a new account, to replay it again.
        </p>
        {source.recording_sha256 !== null && (
          <p>
            Recording SHA-256 <Code>{source.recording_sha256}</Code>
          </p>
        )}
      </Alert>
    );
  } else if (source.mode === 'live') {
    status = <LiveNotice marketData={marketData} />;
  }
  if (status === null && source.coverage_notice === null) {
    return null;
  }
  return (
    <div className={styles.notices}>
      <CoverageNotice source={source} />
      {status}
    </div>
  );
}

// The server's market-data source on every screen: what it is, whether it
// is historical or live, its state and market time, and replay controls.
export function SourceBar() {
  const marketData = useMarketData();
  if (marketData.status !== 'success') {
    return null;
  }
  const { source, stream } = marketData.data;
  const state = SOURCE_STATE[source.state];
  return (
    <>
      <div className={styles.bar} role="region" aria-label="Market-data source">
        <span className={styles.mode} data-mode={source.mode}>
          {source.synthetic ? 'Synthetic replay' : MARKET_DATA_MODE[source.mode]}
        </span>
        <span className={styles.label}>{source.label}</span>
        <Badge tone={state.tone}>{state.label}</Badge>
        {source.replay_speed !== null && (
          <span className={styles.fact}>
            Speed <strong>{source.replay_speed}</strong>
          </span>
        )}
        <span className={styles.fact}>
          Market time{' '}
          <strong>
            {stream.market_time === null ? 'none yet' : <MarketTime value={stream.market_time} />}
          </strong>{' '}
          {stream.market_time !== null && 'UTC'}
        </span>
        <span className={styles.fact}>
          Events <strong>{source.events_applied}</strong>
        </span>
        {source.coverage_label !== null && <span className={styles.coverage}>{source.coverage_label}</span>}
        <ReplayControl state={source.state} />
      </div>
      <SourceNotices marketData={marketData.data} />
    </>
  );
}
