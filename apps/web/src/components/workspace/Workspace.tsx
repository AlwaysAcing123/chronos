import { isAccountNotFound } from '../../api/errors';
import { FAILURE } from '../../presentation/vocabulary';
import { useAccount, usePortfolio } from '../../state/queries';
import accountStyles from '../account/account.module.css';
import { FillHistory } from '../history/FillHistory';
import { OrderBlotter } from '../history/OrderBlotter';
import { OrderTicket } from '../ticket/OrderTicket';
import { Alert } from '../ui/Alert';
import { Button } from '../ui/Button';
import { FailureNotice } from '../ui/FailureNotice';
import { Panel } from '../ui/Panel';
import { MarketPanel } from './MarketPanel';
import { PortfolioPanel } from './PortfolioPanel';
import styles from './workspace.module.css';

const SECTIONS = [
  { id: 'portfolio', label: 'Portfolio' },
  { id: 'ticket', label: 'Trade' },
  { id: 'market', label: 'Market' },
  { id: 'orders', label: 'Orders' },
  { id: 'fills', label: 'Fills' },
] as const;

function AccountNotFound({
  accountId,
  onClose,
  onForget,
}: {
  readonly accountId: string;
  readonly onClose: () => void;
  readonly onForget: (accountId: string) => void;
}) {
  return (
    <div className={accountStyles.notFound}>
      <Alert
        tone="caution"
        title={`${FAILURE.account_not_found.title}: ${accountId}`}
        live
        actions={
          <>
            <Button variant="primary" onClick={onClose}>
              Choose another account
            </Button>
            <Button
              onClick={() => {
                onForget(accountId);
              }}
            >
              Remove from recent accounts
            </Button>
          </>
        }
      >
        {FAILURE.account_not_found.detail}
      </Alert>
    </div>
  );
}

export function Workspace({
  accountId,
  onClose,
  onForget,
}: {
  readonly accountId: string;
  readonly onClose: () => void;
  readonly onForget: (accountId: string) => void;
}) {
  const account = useAccount(accountId);
  const portfolio = usePortfolio(accountId);

  if (isAccountNotFound(account.error) || isAccountNotFound(portfolio.error)) {
    return <AccountNotFound accountId={accountId} onClose={onClose} onForget={onForget} />;
  }
  if (account.status === 'pending') {
    return (
      <Panel title="Paper account">
        <p role="status">Loading paper account {accountId}…</p>
      </Panel>
    );
  }
  if (account.status === 'error' && account.data === undefined) {
    return (
      <Panel title="Paper account">
        <FailureNotice error={account.error} onRetry={() => void account.refetch()} retrying={account.isFetching} />
      </Panel>
    );
  }

  return (
    <div className={styles.workspace}>
      <h1 className="visually-hidden">Paper-trading workspace for {accountId}</h1>
      <nav className={styles.sectionNav} aria-label="Workspace sections">
        {SECTIONS.map((section) => (
          <a key={section.id} href={`#${section.id}`}>
            {section.label}
          </a>
        ))}
      </nav>
      <div className={styles.grid}>
        <PortfolioPanel id="portfolio" className={styles.portfolio} portfolio={portfolio} account={account} />
        <MarketPanel id="market" className={styles.market} instrument={account.data.instrument} portfolio={portfolio} />
        <div className={styles.ticket}>
          <OrderTicket
            id="ticket"
            account={account.data}
            reading={portfolio.status === 'success' ? portfolio.data : null}
          />
        </div>
        <OrderBlotter id="orders" className={styles.orders} accountId={accountId} />
        <FillHistory id="fills" className={styles.fills} accountId={accountId} />
      </div>
    </div>
  );
}
