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
  return (
    <div className={styles.entry}>
      <section className={styles.intro} aria-labelledby="entry-title">
        <h1 id="entry-title" className={styles.title}>
          Open a paper-trading account
        </h1>
        <p className={styles.lede}>
          A paper account is a simulated ledger kept by the Chronos server. It starts with simulated cash, trades one
          instrument long-only, and executes immediate-or-cancel orders against the server’s own market data.
        </p>
        <ul className={styles.points}>
          <li>Fills, fees, and PnL are calculated exactly by the server and stored in PostgreSQL.</li>
          <li>No order ever reaches a broker or exchange, and no real money is involved.</li>
          <li>This browser remembers only which account IDs you opened.</li>
        </ul>
      </section>
      <div className={styles.columns}>
        <CreateAccountForm onOpen={onOpen} />
        <OpenAccountForm recentAccountIds={recentAccountIds} onOpen={onOpen} onForget={onForget} />
      </div>
    </div>
  );
}
