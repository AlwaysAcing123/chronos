import type { UseQueryResult } from '@tanstack/react-query';

import type { PortfolioReading } from '../../api/client';
import type { Account, Portfolio } from '../../api/schemas';
import { ACCOUNT_STATUS, FAILURE } from '../../presentation/vocabulary';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { DecimalText, Unavailable } from '../ui/DecimalText';
import type { Fact } from '../ui/Facts';
import { Facts } from '../ui/Facts';
import { FailureNotice } from '../ui/FailureNotice';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ReasonText } from '../ui/ReasonText';
import styles from './workspace.module.css';

// Durable ledger values, identical in the account and portfolio resources.
type Ledger = Pick<
  Account | Portfolio,
  'status' | 'starting_cash' | 'cash' | 'position' | 'open_cost_basis' | 'average_entry_price' | 'gross_realized_pnl' | 'fees_paid'
>;

function ledgerFacts(ledger: Ledger, version: string): Fact[] {
  const status = ACCOUNT_STATUS[ledger.status];
  return [
    { label: 'Status', value: <Badge tone={status.tone}>{status.label}</Badge> },
    { label: 'Starting cash', value: <DecimalText value={ledger.starting_cash} /> },
    { label: 'Cash', value: <DecimalText value={ledger.cash} /> },
    { label: 'Position', value: <DecimalText value={ledger.position} /> },
    { label: 'Open cost basis', value: <DecimalText value={ledger.open_cost_basis} /> },
    {
      label: 'Average entry price',
      value:
        ledger.average_entry_price === null ? <Unavailable>None (flat)</Unavailable> : (
          <DecimalText value={ledger.average_entry_price} />
        ),
    },
    { label: 'Realized PnL', value: <DecimalText value={ledger.gross_realized_pnl} signed /> },
    { label: 'Fees paid', value: <DecimalText value={ledger.fees_paid} /> },
    { label: 'Account version', value: <span className="mono">{version}</span> },
  ];
}

function Headline({ portfolio }: { readonly portfolio: Portfolio | null }) {
  const items = [
    { label: 'Equity', value: portfolio && <DecimalText value={portfolio.equity} /> },
    { label: 'Total PnL', value: portfolio && <DecimalText value={portfolio.total_pnl} signed /> },
    { label: 'Return', value: portfolio && <DecimalText value={portfolio.return_percent} signed suffix=" %" /> },
  ];
  return (
    <dl className={styles.headline}>
      {items.map((item) => (
        <div key={item.label} className={styles.headlineItem}>
          <dt className={styles.headlineLabel}>{item.label}</dt>
          <dd className={styles.headlineValue}>{item.value ?? <Unavailable />}</dd>
        </div>
      ))}
    </dl>
  );
}

const MARK_POLICY_NOTE =
  'Marked at the best bid. This is mark-to-market valuation, not guaranteed liquidation value: it ignores ' +
  'displayed size, market impact, queue position, latency, and the fee a closing sale would pay.';

function Valued({ portfolio }: { readonly portfolio: Portfolio }) {
  return (
    <>
      <Headline portfolio={portfolio} />
      <div className={styles.group}>
        <h3 className={styles.groupTitle}>Valuation</h3>
        <Facts
          facts={[
            { label: 'Mark price (best bid)', value: <DecimalText value={portfolio.mark_price} /> },
            { label: 'Position market value', value: <DecimalText value={portfolio.position_market_value} /> },
            { label: 'Unrealized PnL', value: <DecimalText value={portfolio.unrealized_pnl} signed /> },
            { label: 'Valued at (market time, UTC)', value: <MarketTime value={portfolio.valued_at} /> },
          ]}
        />
        <p className={styles.note}>{MARK_POLICY_NOTE}</p>
      </div>
      <div className={styles.group}>
        <h3 className={styles.groupTitle}>Account ledger</h3>
        <Facts facts={ledgerFacts(portfolio, portfolio.account_version)} />
      </div>
    </>
  );
}

// The market cannot value the account, so market-dependent values are
// unavailable; the durable ledger comes from the account resource instead.
function NotValued({
  reading,
  account,
}: {
  readonly reading: Extract<PortfolioReading, { kind: 'unavailable' }>;
  readonly account: UseQueryResult<Account, unknown>;
}) {
  const { error, category } = reading;
  return (
    <>
      <Alert tone={category === 'valuation_failed' ? 'negative' : 'caution'} title={FAILURE[category].title}>
        <p>
          <ReasonText error={error} />
        </p>
        <p>Equity, PnL, market value, and return are not shown rather than estimated.</p>
      </Alert>
      <Headline portfolio={null} />
      <div className={styles.group}>
        <h3 className={styles.groupTitle}>Account ledger</h3>
        {account.status === 'success' ? (
          <Facts facts={ledgerFacts(account.data, account.data.version)} />
        ) : account.status === 'error' ? (
          <FailureNotice error={account.error} onRetry={() => void account.refetch()} retrying={account.isFetching} />
        ) : (
          <span className={styles.note}>Loading account…</span>
        )}
      </div>
    </>
  );
}

export function PortfolioPanel({
  id,
  className,
  portfolio,
  account,
}: {
  readonly id?: string | undefined;
  readonly className?: string | undefined;
  readonly portfolio: UseQueryResult<PortfolioReading, unknown>;
  readonly account: UseQueryResult<Account, unknown>;
}) {
  return (
    <Panel id={id} className={className} title="Portfolio" meta="Exact server valuation">
      {portfolio.status === 'pending' && <span className={styles.note}>Loading valuation…</span>}
      {portfolio.status === 'error' && (
        // A failed read shows no values, not the last known ones.
        <FailureNotice error={portfolio.error} onRetry={() => void portfolio.refetch()} retrying={portfolio.isFetching} />
      )}
      {portfolio.status === 'success' &&
        (portfolio.data.kind === 'valued' ? (
          <Valued portfolio={portfolio.data.portfolio} />
        ) : (
          <NotValued reading={portfolio.data} account={account} />
        ))}
    </Panel>
  );
}
