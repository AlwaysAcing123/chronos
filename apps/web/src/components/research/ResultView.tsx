import type { ReactNode } from 'react';
import { useId, useState } from 'react';

import type { AccountName, AccountResult, ResearchResult, Valuation } from '../../api/researchSchemas';
import { isComplete } from '../../api/researchSchemas';
import type { UnsignedInteger } from '../../api/schemas';
import { nanosecondsAsSeconds } from '../../presentation/format';
import {
  ACCOUNT_LABEL,
  MODEL_LABEL,
  WINDOW_LABEL,
  decisionReason,
  ppbAsPercent,
  tradeOutcome,
} from '../../presentation/research';
import { useDecisions, useEquity, useResult, useTrades } from '../../state/research';
import historyStyles from '../history/history.module.css';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { DecimalText, Unavailable } from '../ui/DecimalText';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { EquityChart } from './EquityChart';
import { ResearchFailure } from './Research';
import styles from './research.module.css';

const ACCOUNTS: readonly AccountName[] = ['strategy', 'buy_and_hold', 'cash'];

function seconds(nanoseconds: UnsignedInteger): string {
  return `${nanosecondsAsSeconds(nanoseconds)} s`;
}

function unavailableReason(valuation: Valuation): string {
  if (isComplete(valuation)) {
    return '';
  }
  const instruments = valuation.unavailable.instruments.map((item) => item.reason.replaceAll('_', ' '));
  return instruments.length > 0 ? instruments.join(', ') : valuation.unavailable.code.replaceAll('_', ' ');
}

// One cell of a valued figure: the exact value, or why there is none.
function valued(account: AccountResult, render: (valuation: Exclude<Valuation, { unavailable: unknown }>) => ReactNode) {
  const valuation = account.final.valuation;
  return isComplete(valuation) ? render(valuation) : <Unavailable>{`Unavailable: ${unavailableReason(valuation)}`}</Unavailable>;
}

const ROWS: readonly { readonly label: string; readonly cell: (account: AccountResult) => ReactNode }[] = [
  { label: 'Final equity', cell: (account) => valued(account, (valuation) => <DecimalText value={valuation.equity} />) },
  {
    label: 'Net total PnL',
    cell: (account) => valued(account, (valuation) => <DecimalText value={valuation.net_total_pnl} signed />),
  },
  {
    label: 'Return',
    cell: (account) => valued(account, (valuation) => <DecimalText value={valuation.return_percent} signed suffix="%" />),
  },
  { label: 'Realized PnL before fees', cell: (account) => <DecimalText value={account.final.ledger.gross_realized_pnl} signed /> },
  {
    label: 'Unrealized PnL',
    cell: (account) => valued(account, (valuation) => <DecimalText value={valuation.gross_unrealized_pnl} signed />),
  },
  { label: 'Fees', cell: (account) => <DecimalText value={account.final.ledger.fees_paid} /> },
  { label: 'Fills', cell: (account) => <span className="numeric">{account.fills}</span> },
  {
    label: 'Orders',
    cell: (account) => (
      <span className="numeric">
        {account.orders.scheduled}
        <span className={styles.reasonCode}>
          {account.orders.rejected} rejected · {account.orders.expired} expired
        </span>
      </span>
    ),
  },
  {
    label: 'Turnover',
    cell: (account) => (
      <>
        <DecimalText value={account.turnover.notional} />
        <span className={styles.reasonCode}>{account.turnover.percent_of_starting_cash}% of starting cash</span>
      </>
    ),
  },
  {
    label: 'Maximum drawdown',
    cell: (account) => (
      <>
        <DecimalText value={account.drawdown.maximum} />
        <span className={styles.reasonCode}>
          {account.drawdown.maximum_percent_of_peak === null ? 'no fall from a peak' : `${account.drawdown.maximum_percent_of_peak}% of peak`}
          {account.drawdown.points_unvalued !== '0' && ` · ${account.drawdown.points_unvalued} points unvalued`}
        </span>
      </>
    ),
  },
  { label: 'Final position (shares)', cell: (account) => <DecimalText value={account.final.ledger.position} /> },
  { label: 'Final cash', cell: (account) => <DecimalText value={account.final.ledger.cash} /> },
];

function Outcome({ result }: { readonly result: ResearchResult }) {
  return (
    <Panel title="Outcome" meta={`Starting cash ${result.configuration.starting_cash} for each account`}>
      <div className={historyStyles.scroll}>
        <table className={`${historyStyles.table ?? ''} ${styles.comparison ?? ''}`}>
          <caption className="visually-hidden">Strategy and benchmark outcomes</caption>
          <thead>
            <tr>
              <th scope="col">Figure</th>
              {ACCOUNTS.map((account) => (
                <th key={account} scope="col" className={historyStyles.number}>
                  {ACCOUNT_LABEL[account]}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {ROWS.map((row) => (
              <tr key={row.label}>
                <th scope="row">{row.label}</th>
                {ACCOUNTS.map((account) => (
                  <td key={account} className={historyStyles.number} data-label={ACCOUNT_LABEL[account]}>
                    <div>{row.cell(result.accounts[account])}</div>
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <p className={styles.note}>
        Equity marks an open position at the best bid of the last quote in the window; it is not a sale. Buy and hold
        bought once, at the strategy’s first decision, under the same latency, fills, and fees; cash never trades.
      </p>
    </Panel>
  );
}

function Equity({ runId }: { readonly runId: string }) {
  const equity = useEquity(runId);
  return (
    <Panel title="Equity" meta="After each decision and execution, and at the window’s end">
      {equity.status === 'pending' ? (
        <p role="status">Loading the equity curve…</p>
      ) : equity.status === 'error' ? (
        <ResearchFailure error={equity.error} onRetry={() => void equity.refetch()} retrying={equity.isFetching} />
      ) : (
        <EquityChart points={equity.data} />
      )}
    </Panel>
  );
}

function Trades({ runId }: { readonly runId: string }) {
  const groupId = useId();
  const [account, setAccount] = useState<AccountName>('strategy');
  const trades = useTrades(runId, account);
  const rows = trades.data?.pages.flatMap((page) => page.trades) ?? [];
  return (
    <Panel title="Trades" meta="Orders and their outcomes, oldest first">
      <fieldset className={styles.choices} aria-describedby={groupId}>
        <legend id={groupId}>Account</legend>
        <div className={styles.inline}>
          {(['strategy', 'buy_and_hold'] as const).map((name) => (
            <label key={name} className={styles.choice}>
              <input
                type="radio"
                name={`${groupId}-account`}
                checked={account === name}
                onChange={() => {
                  setAccount(name);
                }}
              />
              {ACCOUNT_LABEL[name]}
            </label>
          ))}
        </div>
      </fieldset>
      {trades.status === 'pending' ? (
        <p role="status">Loading trades…</p>
      ) : trades.status === 'error' ? (
        <ResearchFailure error={trades.error} onRetry={() => void trades.refetch()} retrying={trades.isFetching} />
      ) : rows.length === 0 ? (
        <p className={historyStyles.empty}>This account placed no orders.</p>
      ) : (
        <>
          <div className={historyStyles.scroll}>
            <table className={historyStyles.table}>
              <caption className="visually-hidden">{ACCOUNT_LABEL[account]} orders and outcomes</caption>
              <thead>
                <tr>
                  <th scope="col">Signal (UTC)</th>
                  <th scope="col">Executed (UTC)</th>
                  <th scope="col">Side</th>
                  <th scope="col" className={historyStyles.number}>
                    Quantity
                  </th>
                  <th scope="col">Outcome</th>
                  <th scope="col" className={historyStyles.number}>
                    Price
                  </th>
                  <th scope="col" className={historyStyles.number}>
                    Fee
                  </th>
                  <th scope="col" className={historyStyles.number}>
                    Position after
                  </th>
                  <th scope="col">Detail</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((trade) => {
                  const outcome = tradeOutcome(trade.outcome);
                  return (
                    <tr key={trade.client_order_id}>
                      <td data-label="Signal (UTC)">
                        <MarketTime value={trade.signal_at} stacked />
                      </td>
                      <td data-label="Executed (UTC)">
                        {trade.executed_at === undefined ? (
                          <Unavailable>Never executed</Unavailable>
                        ) : (
                          <MarketTime value={trade.executed_at} stacked />
                        )}
                      </td>
                      <td className={historyStyles.side} data-label="Side">
                        {trade.side === 'buy' ? 'Buy' : 'Sell'}
                      </td>
                      <td className={historyStyles.number} data-label="Quantity">
                        {trade.quantity}
                      </td>
                      <td data-label="Outcome">
                        <Badge tone={outcome.tone}>{outcome.label}</Badge>
                      </td>
                      <td className={historyStyles.number} data-label="Price">
                        {trade.fill ? <DecimalText value={trade.fill.price} /> : <Unavailable>No fill</Unavailable>}
                      </td>
                      <td className={historyStyles.number} data-label="Fee">
                        {trade.fill ? <DecimalText value={trade.fill.fee} /> : <Unavailable>None</Unavailable>}
                      </td>
                      <td className={historyStyles.number} data-label="Position after">
                        {trade.position_after ?? <Unavailable>Unchanged</Unavailable>}
                      </td>
                      <td className={historyStyles.detail} data-label="Detail">
                        <div>
                          {trade.rejection ? (
                            <>
                              Rejected by {trade.rejection.by.replaceAll('_', ' ')}
                              <span className={styles.reasonCode}>{trade.rejection.code}</span>
                            </>
                          ) : trade.reason !== undefined ? (
                            trade.reason.replaceAll('_', ' ')
                          ) : trade.observation ? (
                            <>
                              Quote {trade.observation.ingest_sequence}
                              {trade.fill && trade.fill.quantity !== trade.quantity && (
                                <span className={styles.reasonCode}>
                                  filled {trade.fill.quantity} of {trade.quantity}
                                </span>
                              )}
                            </>
                          ) : null}
                        </div>
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
          <div className={historyStyles.footer}>
            <span>{rows.length} shown</span>
            {trades.hasNextPage && (
              <Button onClick={() => void trades.fetchNextPage()} disabled={trades.isFetchingNextPage}>
                {trades.isFetchingNextPage ? 'Loading…' : 'Load more'}
              </Button>
            )}
          </div>
        </>
      )}
    </Panel>
  );
}

function Decisions({ result }: { readonly result: ResearchResult }) {
  const decisions = useDecisions(result.run_id);
  const rows = (decisions.data?.pages.flatMap((page) => page.decisions) ?? []).filter((row) => row.account === 'strategy');
  const counts = result.decisions;
  return (
    <Panel title="Decisions" meta={`${counts.evaluated} evaluated`}>
      <Facts
        facts={[
          { label: 'Orders scheduled', value: counts.orders },
          { label: 'Already at target', value: counts.already_at_target },
          { label: 'Held: below threshold', value: counts.hold_below_threshold },
          { label: 'Held: flat prediction', value: counts.hold_flat_prediction },
          { label: 'Held: order pending', value: counts.order_pending },
          { label: 'Rows within the decision interval', value: counts.skipped_decision_interval },
          { label: 'Rows without model inputs', value: counts.skipped_prediction_unavailable },
        ]}
      />
      <div className={styles.section}>
        {decisions.status === 'pending' ? (
          <p role="status">Loading decisions…</p>
        ) : decisions.status === 'error' ? (
          <ResearchFailure
            error={decisions.error}
            onRetry={() => void decisions.refetch()}
            retrying={decisions.isFetching}
          />
        ) : (
          <>
            <div className={historyStyles.scroll}>
              <table className={historyStyles.table}>
                <caption className="visually-hidden">Strategy decisions, oldest first</caption>
                <thead>
                  <tr>
                    <th scope="col">Signal (UTC)</th>
                    <th scope="col">Prediction</th>
                    <th scope="col" className={historyStyles.number}>
                      Confidence
                    </th>
                    <th scope="col" className={historyStyles.number}>
                      Position before
                    </th>
                    <th scope="col" className={historyStyles.number}>
                      Target
                    </th>
                    <th scope="col">Decision</th>
                  </tr>
                </thead>
                <tbody>
                  {rows.map((row) => (
                    <tr key={row.decision}>
                      <td data-label="Signal (UTC)">
                        <MarketTime value={row.signal.available_at} stacked />
                      </td>
                      <td data-label="Prediction">{row.signal.class}</td>
                      <td className={historyStyles.number} data-label="Confidence">
                        {ppbAsPercent(row.signal.confidence_ppb)}%
                      </td>
                      <td className={historyStyles.number} data-label="Position before">
                        {row.position_before}
                      </td>
                      <td className={historyStyles.number} data-label="Target">
                        {row.target ?? <Unavailable>Hold</Unavailable>}
                      </td>
                      <td data-label="Decision">{decisionReason(row.reason)}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
            <div className={historyStyles.footer}>
              <span>{rows.length} shown</span>
              {decisions.hasNextPage && (
                <Button onClick={() => void decisions.fetchNextPage()} disabled={decisions.isFetchingNextPage}>
                  {decisions.isFetchingNextPage ? 'Loading…' : 'Load more'}
                </Button>
              )}
            </div>
          </>
        )}
      </div>
    </Panel>
  );
}

function Provenance({ result }: { readonly result: ResearchResult }) {
  const { configuration, source, window } = result;
  const id = (value: string) => <span className={styles.ids}>{value}</span>;
  return (
    <Panel title="Assumptions and provenance">
      <h3 className={styles.sectionTitle}>Configuration</h3>
      <Facts
        facts={[
          { label: 'Policy', value: `${configuration.policy.name} v${configuration.policy.version}` },
          { label: 'Confidence threshold', value: `${ppbAsPercent(configuration.policy.threshold_ppb)}%` },
          { label: 'Target position (shares)', value: configuration.policy.target_quantity },
          { label: 'Decision interval', value: seconds(configuration.policy.decision_interval_ns) },
          { label: 'Latency', value: seconds(configuration.execution.latency_ns) },
          { label: 'Orders', value: `${configuration.execution.order_type}, ${configuration.execution.time_in_force.replaceAll('_', ' ')}` },
          { label: 'Fee per fill', value: configuration.execution.fee_per_fill },
          { label: 'Maximum quote age', value: seconds(configuration.execution.maximum_quote_age_ns) },
          { label: 'Maximum order notional', value: configuration.execution.maximum_order_notional },
          { label: 'Starting cash', value: configuration.starting_cash },
        ]}
      />
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Assumptions</h3>
        <ul className={styles.list}>
          {result.assumptions.map((text) => (
            <li key={text}>{text}</li>
          ))}
        </ul>
      </div>
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Definitions</h3>
        <Facts facts={Object.entries(result.definitions).map(([label, value]) => ({ label: label.replaceAll('_', ' '), value }))} />
      </div>
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Window</h3>
        <Facts
          facts={[
            { label: 'Window', value: WINDOW_LABEL[window.name] },
            { label: 'Starts', value: <MarketTime value={window.start_at} stacked /> },
            {
              label: 'Ends',
              value: window.end_at === null ? 'End of the data' : <MarketTime value={window.end_at} stacked />,
            },
            {
              label: 'Valued at',
              value: window.valued_at === null ? <Unavailable /> : <MarketTime value={window.valued_at} stacked />,
            },
            { label: 'Training labels known by', value: <MarketTime value={source.training_labels_available_through} stacked /> },
            { label: 'Warm-up events', value: result.events.warmup },
            { label: 'Events in the window', value: result.events.window },
            { label: 'Events after it, unused', value: result.events.after_window_unused },
          ]}
        />
      </div>
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Provenance</h3>
        <Facts
          facts={[
            { label: 'Result', value: id(result.run_id) },
            { label: 'Schedule', value: id(source.schedule_id) },
            { label: 'Model', value: `${MODEL_LABEL[source.model]}, ${source.instrument}` },
            { label: 'Experiment', value: id(source.experiment_id) },
            { label: 'Parameters SHA-256', value: id(source.parameters_sha256) },
            { label: 'Dataset', value: id(source.dataset_id) },
            { label: 'Features', value: id(source.feature_artifact_id) },
            { label: 'Labels', value: id(source.label_artifact_id) },
            { label: 'Split', value: id(source.split_artifact_id) },
            { label: 'Stream', value: id(source.stream) },
            { label: 'Runner', value: `${result.runner.name} v${result.runner.version}` },
            ...Object.entries(result.environment).map(([label, value]) => ({ label, value })),
          ]}
        />
      </div>
    </Panel>
  );
}

export function ResultView({ runId }: { readonly runId: string }) {
  const result = useResult(runId);
  if (result.status === 'pending') {
    return (
      <Panel title="Result">
        <p role="status">Loading the result…</p>
      </Panel>
    );
  }
  if (result.status === 'error') {
    return (
      <Panel title="Result">
        <ResearchFailure
          error={result.error}
          title="This result cannot be shown"
          onRetry={() => void result.refetch()}
          retrying={result.isFetching}
        />
      </Panel>
    );
  }
  const data = result.data;
  return (
    <>
      <Alert tone="info" title={`${data.source.instrument} · ${WINDOW_LABEL[data.window.name]} window · ${MODEL_LABEL[data.source.model]}`}>
        <p>
          {data.scope.charAt(0).toUpperCase() + data.scope.slice(1)}. Prediction accuracy is reported with the model,
          not here: these are simulated trading results. <Code>{data.run_id}</Code>
        </p>
      </Alert>
      <Outcome result={data} />
      <Equity runId={runId} />
      <Trades runId={runId} />
      <Decisions result={data} />
      <Provenance result={data} />
    </>
  );
}
