import type { Health } from '../../api/schemas';
import { formatLocalClock } from '../../presentation/format';
import type { Labelled } from '../../presentation/vocabulary';
import { useHealth } from '../../state/queries';
import type { RefreshSummary } from '../../state/refreshSummary';
import { useRefreshSummary } from '../../state/refreshSummary';
import { useChronosServices } from '../../state/services';
import { Badge } from '../ui/Badge';
import { MarketTime } from '../ui/MarketTime';
import styles from './shell.module.css';

function apiStatus(health: ReturnType<typeof useHealth>): Labelled {
  if (health.status === 'error') {
    return { label: 'API unreachable', tone: 'negative' };
  }
  if (health.status === 'pending') {
    return { label: 'Connecting', tone: 'neutral' };
  }
  return health.data.persistence === 'available'
    ? { label: 'API online', tone: 'positive' }
    : { label: 'Database unavailable', tone: 'negative' };
}

function marketStatus(health: Health | undefined): Labelled | null {
  if (health === undefined) {
    return null;
  }
  const market = health.market_data;
  if (market.state === 'interrupted') {
    return { label: 'Market stream interrupted', tone: 'negative' };
  }
  return market.last_ingest_sequence === null
    ? { label: 'No market data', tone: 'caution' }
    : { label: 'Market stream accepting', tone: 'info' };
}

function refreshText({ confirmedAt, failingReads }: RefreshSummary): string {
  if (confirmedAt !== null) {
    return ` · last refresh ${formatLocalClock(confirmedAt)}`;
  }
  return failingReads > 0 ? ' · refresh failing' : ' · not yet refreshed';
}

export function ConnectionStatus() {
  const health = useHealth();
  const refresh = useRefreshSummary();
  const { refresh: policy } = useChronosServices();
  const api = apiStatus(health);
  const current = health.status === 'success' ? health.data : undefined;
  const market = marketStatus(current);
  const marketTime = current?.market_data.market_time ?? null;

  return (
    <div className={styles.status} role="group" aria-label="Server status">
      <span className={styles.statusItem}>
        <Badge tone={api.tone}>{api.label}</Badge>
      </span>
      {market && (
        <span className={styles.statusItem}>
          <Badge tone={market.tone}>{market.label}</Badge>
          {marketTime && (
            <span>
              Market time <MarketTime value={marketTime} /> UTC
            </span>
          )}
        </span>
      )}
      <span className={styles.refresh}>
        <span className={styles.refreshDot} data-refreshing={String(refresh.refreshing)} aria-hidden="true" />
        <span>
          {policy.accountMs === false ? 'Not polling' : `Polling every ${String(policy.accountMs / 1000)} s`}
          {refreshText(refresh)}
          {refresh.failingReads > 0 &&
            ` · ${String(refresh.failingReads)} ${refresh.failingReads === 1 ? 'read' : 'reads'} failing`}
        </span>
      </span>
    </div>
  );
}
