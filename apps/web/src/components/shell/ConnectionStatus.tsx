import { formatLocalClock } from '../../presentation/format';
import type { Labelled } from '../../presentation/vocabulary';
import { useHealth } from '../../state/queries';
import type { RefreshSummary } from '../../state/refreshSummary';
import { useRefreshSummary } from '../../state/refreshSummary';
import { useChronosServices } from '../../state/services';
import type { StreamStatus } from '../../state/streamStatus';
import { useStreamStatus } from '../../state/streamStatus';
import { Badge } from '../ui/Badge';
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

// How the page learns about changes: never described as more live than it is.
function updatesText(stream: StreamStatus, marketMs: number | false): string {
  switch (stream) {
    case 'streaming':
      return 'Streaming changes';
    case 'connecting':
      return 'Connecting to change stream';
    case 'polling':
      return marketMs === false ? 'Not polling' : `Polling every ${String(marketMs / 1000)} s`;
  }
}

function refreshText({ confirmedAt, failingReads }: RefreshSummary): string {
  const last =
    confirmedAt !== null
      ? ` · last refresh ${formatLocalClock(confirmedAt)}`
      : failingReads > 0
        ? ' · refresh failing'
        : ' · not yet refreshed';
  const failing =
    failingReads > 0 ? ` · ${String(failingReads)} ${failingReads === 1 ? 'read' : 'reads'} failing` : '';
  return confirmedAt !== null ? last + failing : last;
}

export function ConnectionStatus() {
  const health = useHealth();
  const refresh = useRefreshSummary();
  const stream = useStreamStatus();
  const { refresh: policy } = useChronosServices();
  const api = apiStatus(health);

  return (
    <div className={styles.status} role="group" aria-label="Server status">
      <span className={styles.statusItem}>
        <Badge tone={api.tone}>{api.label}</Badge>
      </span>
      <span className={styles.refresh}>
        <span className={styles.refreshDot} data-refreshing={String(refresh.refreshing)} aria-hidden="true" />
        <span>
          {updatesText(stream, policy.marketMs)}
          {refreshText(refresh)}
        </span>
      </span>
    </div>
  );
}
