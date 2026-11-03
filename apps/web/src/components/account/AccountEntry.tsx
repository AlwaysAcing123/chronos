import { useMarketData } from '../../state/queries';
import { CreateAccountForm } from './CreateAccountForm';
import { OpenAccountForm } from './OpenAccountForm';
import styles from './account.module.css';

export function AccountEntry({
  recentAccountIds,
  onOpen,
  onForget,
}: {
  readonly recentAccountIds: readonly string[];
  readonly onOpen: (accountId: string) => void;
  readonly onForget: (accountId: string) => void;
}) {
  const marketData = useMarketData();
  // A synthetic replay is never called market data.
  const synthetic = marketData.status === 'success' && marketData.data.source.synthetic;
  return (
    <div className={styles.entry}>
      <section className={styles.intro} aria-labelledby="entry-title">
        <p className={styles.eyebrow}>Chronos / Paper environment</p>
        <h1 id="entry-title" className={styles.title}>
          Open a paper-trading account
        </h1>
        <p className={styles.lede}>
          {synthetic ? 'A simulated ledger over invented, synthetic prices.' : 'A simulated ledger, real market data.'}{' '}
          Trade any available instrument long-only from one shared cash balance, with immediate orders or
          good-til-cancelled limits. The Chronos server handles execution.
        </p>
        <ul className={styles.points}>
          <li>Fills, fees, and PnL are calculated exactly by the server and stored in PostgreSQL.</li>
          <li>No order ever reaches a broker or exchange, and no real money is involved.</li>
          <li>This browser remembers only which account IDs you opened and which symbol you last viewed.</li>
        </ul>
      </section>
      <div className={styles.columns}>
        <CreateAccountForm onOpen={onOpen} />
        <OpenAccountForm recentAccountIds={recentAccountIds} onOpen={onOpen} onForget={onForget} />
      </div>
    </div>
  );
}
