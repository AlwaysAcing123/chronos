import { useAccount } from '../../state/queries';
import { Button } from '../ui/Button';
import styles from './shell.module.css';

// The open account in the header, with a way back to account selection.
export function AccountContext({ accountId, onSwitch }: { readonly accountId: string; readonly onSwitch: () => void }) {
  const account = useAccount(accountId);
  return (
    <div className={styles.accountContext}>
      <span className={styles.accountLabel}>Paper account</span>
      <span className={styles.accountId}>{accountId}</span>
      {account.status === 'success' && <span className={styles.instrument}>{account.data.instrument}</span>}
      <Button variant="quiet" onClick={onSwitch}>
        Switch account
      </Button>
    </div>
  );
}
