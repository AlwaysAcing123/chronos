import { isAccountNotFound } from '../../api/errors';
import { FAILURE } from '../../presentation/vocabulary';
import type { Account } from '../../api/schemas';
import { useSelectedInstrument } from '../../state/instrumentSelection';
import { useAccount, useHealth, useMarketData, usePortfolio } from '../../state/queries';
import { researchHref } from '../../state/route';
import accountStyles from '../account/account.module.css';
import { FillHistory } from '../history/FillHistory';
import { OrderBlotter } from '../history/OrderBlotter';
import { WorkingOrders } from '../orders/WorkingOrders';
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
  { id: 'working', label: 'Working' },
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

// Every symbol the account could have history in: the configured ones, then
// any it traded that the server no longer offers.
function historySymbols(tradeable: readonly string[], account: Account): string[] {
  const traded = [...account.positions, ...account.closed_positions].map((position) => position.instrument);
  return [...tradeable, ...traded.filter((symbol) => !tradeable.includes(symbol)).sort()];
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
  const health = useHealth();
  const marketData = useMarketData();
  // Configuration order, from whichever resource has loaded.
  const tradeable =
    marketData.data?.instruments.map((item) => item.instrument) ?? health.data?.tradeable_instruments ?? [];
  const { selected, select } = useSelectedInstrument(tradeable);

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
      <div className={styles.heading}>
        <div>
          <p className={styles.eyebrow}>Execution desk</p>
          <h1 className={styles.title}>
            Trading workspace <span className="visually-hidden">for {accountId}</span>
          </h1>
        </div>
        <p className={styles.workspaceNote}>Account overview and paper execution</p>
      </div>
      <nav className={styles.sectionNav} aria-label="Workspace sections">
        {SECTIONS.map((section) => (
          <a key={section.id} href={`#${section.id}`}>
            {section.label}
          </a>
        ))}
      </nav>
      <div className={styles.grid}>
        <PortfolioPanel id="portfolio" className={styles.portfolio} portfolio={portfolio} account={account} />
        <MarketPanel
          id="market"
          className={styles.market}
          selected={selected}
          onSelect={select}
          positions={account.data.positions}
        />
        <div className={styles.ticket}>
          {account.data.owner === null ? (
            <OrderTicket
              id="ticket"
              account={account.data}
              symbols={tradeable}
              selected={selected}
              onSelect={select}
              quotes={marketData.status === 'success' ? marketData.data.instruments : null}
            />
          ) : (
            // The server refuses manual orders on an owned account, so none
            // is offered.
            <Panel id="ticket" title="Order ticket">
              <Alert tone="info" title={`Traded by model deployment ${account.data.owner.deployment_id}`}>
                <p>
                  Only the deployment places orders in this account; manual orders and cancellations are refused.
                  Positions, orders, and fills below are the deployment&rsquo;s.
                </p>
                <p>
                  <a href={researchHref()}>Manage the deployment in Research</a>
                </p>
              </Alert>
            </Panel>
          )}
        </div>
        <WorkingOrders id="working" className={styles.working} accountId={accountId} />
        <OrderBlotter
          id="orders"
          className={styles.orders}
          accountId={accountId}
          symbols={historySymbols(tradeable, account.data)}
        />
        <FillHistory
          id="fills"
          className={styles.fills}
          accountId={accountId}
          symbols={historySymbols(tradeable, account.data)}
        />
      </div>
    </div>
  );
}
