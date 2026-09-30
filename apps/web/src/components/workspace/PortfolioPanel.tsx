import type { UseQueryResult } from '@tanstack/react-query';

import type { PortfolioReading } from '../../api/client';
import type { Account, ClosedPosition, OpenPosition, Portfolio, PositionMarkFailure, ValuedPosition } from '../../api/schemas';
import { ACCOUNT_STATUS, FAILURE, reasonLabel } from '../../presentation/vocabulary';
import type { Column } from '../history/HistoryParts';
import { HistoryTable } from '../history/HistoryParts';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { DecimalText, Unavailable } from '../ui/DecimalText';
import type { Fact } from '../ui/Facts';
import { Facts } from '../ui/Facts';
import { FailureNotice } from '../ui/FailureNotice';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ReasonText } from '../ui/ReasonText';
import styles from './workspace.module.css';

// Durable account totals, identical in the account and portfolio resources.
type Ledger = Pick<
  Account | Portfolio,
  | 'status'
  | 'starting_cash'
  | 'cash'
  | 'reserved_cash'
  | 'available_cash'
  | 'open_cost_basis'
  | 'gross_realized_pnl'
  | 'fees_paid'
>;

function ledgerFacts(ledger: Ledger, version: string): Fact[] {
  const status = ACCOUNT_STATUS[ledger.status];
  return [
    { label: 'Status', value: <Badge tone={status.tone}>{status.label}</Badge> },
    { label: 'Starting cash', value: <DecimalText value={ledger.starting_cash} /> },
    { label: 'Cash', value: <DecimalText value={ledger.cash} /> },
    // Reservations never change cash; they only limit what new orders use.
    { label: 'Reserved by working orders', value: <DecimalText value={ledger.reserved_cash} /> },
    { label: 'Available cash', value: <DecimalText value={ledger.available_cash} /> },
    { label: 'Open cost basis', value: <DecimalText value={ledger.open_cost_basis} /> },
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
  'Each position is marked at its best bid. This is mark-to-market valuation, not guaranteed liquidation value: ' +
  'it ignores displayed size, market impact, queue position, latency, and the fee a closing sale would pay.';

// The ledger columns every open position has, marked or not.
const LEDGER_COLUMNS: readonly Column<OpenPosition>[] = [
  { header: 'Symbol', cell: (position) => <span className={styles.symbol}>{position.instrument}</span> },
  { header: 'Quantity', numeric: true, cell: (position) => <DecimalText value={position.quantity} /> },
  { header: 'Reserved', numeric: true, cell: (position) => <DecimalText value={position.reserved_quantity} /> },
  { header: 'Available', numeric: true, cell: (position) => <DecimalText value={position.available_quantity} /> },
  { header: 'Avg entry', numeric: true, cell: (position) => <DecimalText value={position.average_entry_price} /> },
  { header: 'Cost basis', numeric: true, cell: (position) => <DecimalText value={position.open_cost_basis} /> },
];

const PAID_COLUMNS: readonly Column<OpenPosition>[] = [
  {
    header: 'Realized PnL',
    numeric: true,
    cell: (position) => <DecimalText value={position.gross_realized_pnl} signed />,
  },
  { header: 'Fees', numeric: true, cell: (position) => <DecimalText value={position.fees_paid} /> },
];

const VALUED_COLUMNS: readonly Column<ValuedPosition>[] = [
  ...LEDGER_COLUMNS,
  { header: 'Mark (bid)', numeric: true, cell: (position) => <DecimalText value={position.mark_price} /> },
  { header: 'Market value', numeric: true, cell: (position) => <DecimalText value={position.market_value} /> },
  {
    header: 'Unrealized PnL',
    numeric: true,
    cell: (position) => <DecimalText value={position.unrealized_pnl} signed />,
  },
  ...PAID_COLUMNS,
];

// Without a valuation the market-dependent cells say so; they are never zero.
const UNVALUED_COLUMNS: readonly Column<OpenPosition>[] = [
  ...LEDGER_COLUMNS,
  { header: 'Mark (bid)', numeric: true, cell: () => <Unavailable /> },
  { header: 'Market value', numeric: true, cell: () => <Unavailable /> },
  { header: 'Unrealized PnL', numeric: true, cell: () => <Unavailable /> },
  ...PAID_COLUMNS,
];

const CLOSED_COLUMNS: readonly Column<ClosedPosition>[] = [
  { header: 'Symbol', cell: (position) => <span className={styles.symbol}>{position.instrument}</span> },
  {
    header: 'Realized PnL',
    numeric: true,
    cell: (position) => <DecimalText value={position.gross_realized_pnl} signed />,
  },
  { header: 'Fees', numeric: true, cell: (position) => <DecimalText value={position.fees_paid} /> },
  { header: 'Last fill (UTC)', cell: (position) => <MarketTime value={position.last_fill_at} stacked /> },
];

function Positions<Row extends OpenPosition>({
  positions,
  columns,
}: {
  readonly positions: readonly Row[];
  readonly columns: readonly Column<Row>[];
}) {
  return (
    <div className={styles.group}>
      <h3 className={styles.groupTitle}>Open positions</h3>
      {positions.length === 0 ? (
        <span className={styles.note}>No open positions.</span>
      ) : (
        <HistoryTable
          label="Open positions"
          caption="Open positions by symbol"
          columns={columns}
          rows={positions}
          rowKey={(position) => position.instrument}
        />
      )}
    </div>
  );
}

// Closed positions keep what they realized and paid, which the totals include.
function ClosedPositions({ positions }: { readonly positions: readonly ClosedPosition[] }) {
  if (positions.length === 0) {
    return null;
  }
  return (
    <div className={styles.group}>
      <h3 className={styles.groupTitle}>Closed positions</h3>
      <HistoryTable
        label="Closed positions"
        caption="Closed positions by symbol, with realized PnL and fees"
        columns={CLOSED_COLUMNS}
        rows={positions}
        rowKey={(position) => position.instrument}
      />
    </div>
  );
}

function Valued({ portfolio }: { readonly portfolio: Portfolio }) {
  return (
    <>
      <Headline portfolio={portfolio} />
      <div className={styles.group}>
        <h3 className={styles.groupTitle}>Valuation</h3>
        <Facts
          facts={[
            { label: 'Market value', value: <DecimalText value={portfolio.market_value} /> },
            { label: 'Unrealized PnL', value: <DecimalText value={portfolio.unrealized_pnl} signed /> },
            {
              label: 'Valued at (market time, UTC)',
              value:
                portfolio.valued_at === null ? (
                  <Unavailable>No market events yet</Unavailable>
                ) : (
                  <MarketTime value={portfolio.valued_at} />
                ),
            },
          ]}
        />
        <p className={styles.note}>{MARK_POLICY_NOTE}</p>
      </div>
      <Positions positions={portfolio.positions} columns={VALUED_COLUMNS} />
      <ClosedPositions positions={portfolio.closed_positions} />
      <div className={styles.group}>
        <h3 className={styles.groupTitle}>Account ledger</h3>
        <Facts facts={ledgerFacts(portfolio, portfolio.account_version)} />
      </div>
    </>
  );
}

function MarkFailures({ failures }: { readonly failures: readonly PositionMarkFailure[] }) {
  return (
    <ul className={styles.markFailures} aria-label="Positions without a usable mark">
      {failures.map((failure) => (
        <li key={failure.instrument}>
          <span className={styles.symbol}>{failure.instrument}</span>: {reasonLabel(failure.reason)}{' '}
          <Code>{failure.reason}</Code>
        </li>
      ))}
    </ul>
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
  const failures = error.instruments ?? [];
  return (
    <>
      <Alert tone={category === 'valuation_failed' ? 'negative' : 'caution'} title={FAILURE[category].title}>
        {failures.length > 0 ? (
          <>
            <p>
              {failures.length === 1 ? 'A held position has' : `${String(failures.length)} held positions have`} no usable
              quote, so the portfolio cannot be valued exactly:
            </p>
            <MarkFailures failures={failures} />
          </>
        ) : (
          <p>
            <ReasonText error={error} />
          </p>
        )}
        <p>Equity, PnL, market value, and return are not shown rather than estimated.</p>
      </Alert>
      <Headline portfolio={null} />
      {account.status === 'success' ? (
        <>
          <Positions positions={account.data.positions} columns={UNVALUED_COLUMNS} />
          <ClosedPositions positions={account.data.closed_positions} />
          <div className={styles.group}>
            <h3 className={styles.groupTitle}>Account ledger</h3>
            <Facts facts={ledgerFacts(account.data, account.data.version)} />
          </div>
        </>
      ) : account.status === 'error' ? (
        <FailureNotice error={account.error} onRetry={() => void account.refetch()} retrying={account.isFetching} />
      ) : (
        <span className={styles.note}>Loading account…</span>
      )}
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
