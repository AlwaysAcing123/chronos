import type { ReactNode } from 'react';

import type { ModelName } from '../../api/researchSchemas';
import type {
  Cell,
  CellAccount,
  Delta,
  Scores,
  Study,
  StudyArtifacts,
  StudyFold,
  StudyModel,
  Totals,
  TradingAggregate,
} from '../../api/walkForwardSchemas';
import { notScoredReason } from '../../presentation/comparisons';
import { sentence } from '../../presentation/models';
import { MODEL_LABEL, shortId } from '../../presentation/research';
import type { Check, Verification } from '../../presentation/walkForward';
import {
  CELL_STATE,
  CHECK,
  EVIDENCE,
  MODEL_STATE,
  STUDY_STATUS,
  checkOf,
  foldLabel,
  missingItem,
  scenarioLabel,
  studyReason,
  studyVerification,
} from '../../presentation/walkForward';
import { researchHref, walkForwardHref } from '../../state/route';
import { useStudy } from '../../state/walkForward';
import historyStyles from '../history/history.module.css';
import { MetricValue } from '../research/MetricValue';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { DecimalText, Unavailable } from '../ui/DecimalText';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { SyntheticNotice } from './PlanReview';
import { ScrollingTable } from './ScrollingTable';

const MODELS: readonly ModelName[] = ['majority', 'logistic'];

function Ids({ children }: { readonly children: string }) {
  return <span className={styles.ids}>{children}</span>;
}

function modelOf(fold: StudyFold, name: ModelName): StudyModel {
  return fold.models[name];
}

// --- Trading, per fold first ----------------------------------------------------------------------------

type CompletedCell = Extract<Cell, { readonly state: 'completed' }>;

function money(account: CellAccount, key: 'net_total_pnl' | 'fees_paid', signed = false): ReactNode {
  return 'unavailable' in account.valuation ? (
    <Unavailable>{`Unavailable: ${account.valuation.unavailable.code.replaceAll('_', ' ')}`}</Unavailable>
  ) : (
    <DecimalText value={account.valuation[key]} signed={signed} />
  );
}

function returnOf(account: CellAccount): ReactNode {
  return 'unavailable' in account.valuation ? (
    <Unavailable>Unavailable</Unavailable>
  ) : (
    <DecimalText value={account.valuation.return_percent} signed suffix="%" />
  );
}

function delta(value: Delta): ReactNode {
  return 'unavailable' in value ? (
    <Unavailable>{studyReason(value.unavailable)}</Unavailable>
  ) : (
    <span className={styles.stackedFigures}>
      <DecimalText value={value.net_total_pnl} signed />
      <span className={styles.reasonCode}>
        <DecimalText value={value.return_percent} signed suffix=" pp" />
      </span>
    </span>
  );
}

const CELL_FIGURES: readonly { readonly label: string; readonly figure: (cell: CompletedCell) => ReactNode }[] = [
  { label: 'Net PnL (USD)', figure: (cell) => money(cell.accounts.strategy, 'net_total_pnl', true) },
  { label: 'Return', figure: (cell) => returnOf(cell.accounts.strategy) },
  { label: 'Fees paid (USD)', figure: (cell) => money(cell.accounts.strategy, 'fees_paid') },
  { label: 'Turnover (USD)', figure: (cell) => <DecimalText value={cell.accounts.strategy.turnover.notional} /> },
  { label: 'Fills', figure: (cell) => <span className="numeric">{cell.accounts.strategy.fills}</span> },
  {
    label: 'Orders filled · rejected · expired',
    figure: (cell) => {
      const orders = cell.accounts.strategy.orders;
      return (
        <span className="numeric">
          {orders.filled} · {orders.rejected} · {orders.expired}
        </span>
      );
    },
  },
  { label: 'Largest drawdown (USD)', figure: (cell) => <DecimalText value={cell.accounts.strategy.drawdown.maximum} /> },
  { label: 'Minus buy and hold', figure: (cell) => delta(cell.deltas.buy_and_hold) },
  { label: 'Minus cash', figure: (cell) => delta(cell.deltas.cash) },
];

function CellState({
  cell,
  model,
  trusted,
}: {
  readonly cell: Cell;
  readonly model: StudyModel;
  readonly trusted: boolean;
}) {
  const state = CELL_STATE[cell.state];
  if (cell.state === 'completed' && !trusted) {
    return <Badge tone="caution">Completed, unverified</Badge>;
  }
  return (
    <>
      <Badge tone={state.tone}>{state.label}</Badge>
      {cell.state !== 'completed' && (
        <span className={styles.reasonCode}>
          {studyReason(model.state === 'unevaluable' || model.state === 'failed' ? model.reason : cell.reason)}
        </span>
      )}
    </>
  );
}

// A run's check, read again now, with why it fails.
function CheckBadge({ check }: { readonly check: Check }) {
  return (
    <span>
      <Badge tone={CHECK[check.state].tone}>{CHECK[check.state].label}</Badge>
      {check.state === 'invalid' && <span className={styles.reasonCode}>{check.reason}</span>}
    </span>
  );
}

function TradingByFold({ study, verification }: { readonly study: Study; readonly verification: Verification }) {
  return (
    <Panel title="Trading results by fold" meta="Strategy account of each run">
      <ScrollingTable label="Trading results by fold, scrollable">
        <table className={historyStyles.table}>
          <caption className="visually-hidden">
            Each fold, model, and scenario's strategy results, one row per engine run
          </caption>
          <thead>
            <tr>
              <th scope="col">Fold</th>
              <th scope="col">Model</th>
              <th scope="col">Scenario</th>
              <th scope="col">Run</th>
              <th scope="col">Verification</th>
              {CELL_FIGURES.map((figure) => (
                <th key={figure.label} scope="col">
                  {figure.label}
                </th>
              ))}
              <th scope="col">Journal</th>
            </tr>
          </thead>
          <tbody>
            {study.folds.flatMap((fold) =>
              MODELS.flatMap((name) => {
                const model = modelOf(fold, name);
                return model.cells.map((cell) => {
                  const key = `${fold.index}|${name}|${cell.scenario}`;
                  const check = verification.runs.get(key);
                  return (
                  <tr key={key}>
                    <td data-label="Fold">{foldLabel(fold.index, fold.evaluation.stream)}</td>
                    <td data-label="Model">{MODEL_LABEL[name]}</td>
                    <td data-label="Scenario">
                      <span>
                        {scenarioLabel(cell.scenario)}
                        <span className={styles.reasonCode}>
                          {cell.fee_per_fill} USD · {cell.latency_ns} ns
                        </span>
                      </span>
                    </td>
                    <td data-label="Run">
                      <span>
                        <CellState cell={cell} model={model} trusted={verification.trusted.get(key) === true} />
                      </span>
                    </td>
                    <td data-label="Verification">
                      {check === undefined ? <Unavailable>No run</Unavailable> : <CheckBadge check={check} />}
                    </td>
                    {CELL_FIGURES.map((figure) => (
                      <td key={figure.label} data-label={figure.label} className={historyStyles.number}>
                        {cell.state === 'completed' ? figure.figure(cell) : <Unavailable>Not available</Unavailable>}
                      </td>
                    ))}
                    <td data-label="Journal">
                      {cell.state === 'completed' ? (
                        <a href={researchHref(cell.run.artifact_id)}>Open run {shortId(cell.run.artifact_id)}</a>
                      ) : (
                        <Unavailable>No run</Unavailable>
                      )}
                    </td>
                  </tr>
                  );
                });
              }),
            )}
          </tbody>
        </table>
      </ScrollingTable>
      <p className={styles.note}>
        Every row is its own engine run in new strategy, cash, and buy-and-hold accounts, each starting with{' '}
        {study.configuration.starting_cash} USD. Differences are the research service’s exact figures; return
        differences are in percentage points of that run’s starting cash. Verification is each run’s journal and
        manifest read again now; a completed run resting on anything that does not verify is shown as recorded,
        and labelled unverified.
      </p>
    </Panel>
  );
}

// --- Aggregates -----------------------------------------------------------------------------------------

const TOTAL_FIGURES: readonly { readonly label: string; readonly figure: (totals: Totals) => ReactNode }[] = [
  { label: 'Starting capital, summed (USD)', figure: (totals) => <DecimalText value={totals.starting_capital} /> },
  { label: 'Net PnL (USD)', figure: (totals) => <DecimalText value={totals.accounts.strategy.net_total_pnl} signed /> },
  {
    label: 'Return on summed capital',
    figure: (totals) => <DecimalText value={totals.accounts.strategy.return_percent} signed suffix="%" />,
  },
  { label: 'Fees paid (USD)', figure: (totals) => <DecimalText value={totals.accounts.strategy.fees_paid} /> },
  { label: 'Turnover (USD)', figure: (totals) => <DecimalText value={totals.accounts.strategy.turnover} /> },
  { label: 'Fills', figure: (totals) => <span className="numeric">{totals.accounts.strategy.fills}</span> },
  {
    label: 'Minus buy and hold',
    figure: (totals) => delta(totals.deltas.buy_and_hold),
  },
  { label: 'Minus cash', figure: (totals) => delta(totals.deltas.cash) },
];

/** Which planned folds an aggregate includes: "1 of 2 folds (fold 2)". */
function coverage(folds: TradingAggregate['folds']): string {
  const named = folds.included.length === 1 ? 'fold' : 'folds';
  return `${String(folds.included.length)} of ${folds.planned} folds (${named} ${folds.included.join(', ')})`;
}

/** The included folds whose run, schedule, or fold models do not verify. */
function unverifiedNote(verification: Verification, name: ModelName, scenario: string, folds: readonly string[]) {
  const untrusted = folds.filter((fold) => verification.trusted.get(`${fold}|${name}|${scenario}`) !== true);
  if (untrusted.length === 0) {
    return null;
  }
  return (
    <>
      <Badge tone="negative">Unverified</Badge>
      {untrusted.map((fold) => {
        const run = verification.runs.get(`${fold}|${name}|${scenario}`);
        const why = run?.state === 'unchecked' ? 'was not checked' : 'does not verify';
        return (
          <span key={fold} className={styles.reasonCode}>
            {run?.state === 'verified' ? `includes fold ${fold}, whose schedule or fold models do not verify` : `includes fold ${fold}’s run, which ${why}`}
          </span>
        );
      })}
    </>
  );
}

function TotalsRows({
  name,
  scenario,
  aggregate,
  verification,
}: {
  readonly name: ModelName;
  readonly scenario: string;
  readonly aggregate: TradingAggregate;
  readonly verification: Verification;
}) {
  const identity = (kind: ReactNode, folds: readonly string[] = []) => (
    <>
      <td data-label="Model">{MODEL_LABEL[name]}</td>
      <td data-label="Scenario">{scenarioLabel(scenario)}</td>
      <td data-label="Coverage">
        <span>
          {kind}
          {unverifiedNote(verification, name, scenario, folds)}
        </span>
      </td>
    </>
  );
  const { complete, partial } = aggregate;
  return (
    <>
      {'unavailable' in complete ? (
        <tr>
          {identity(
            <>
              <Badge tone="caution">Complete total unavailable</Badge>
              {complete.missing.map((entry) => (
                <span key={entry.fold} className={styles.reasonCode}>
                  Fold {entry.fold}: {studyReason(entry.reason)}
                </span>
              ))}
            </>,
          )}
          {TOTAL_FIGURES.map((figure) => (
            <td key={figure.label} data-label={figure.label} className={historyStyles.number}>
              <Unavailable>Unavailable</Unavailable>
            </td>
          ))}
        </tr>
      ) : (
        <tr>
          {identity(<Badge tone="positive">{`Complete: all ${aggregate.folds.planned} folds`}</Badge>, aggregate.folds.included)}
          {TOTAL_FIGURES.map((figure) => (
            <td key={figure.label} data-label={figure.label} className={historyStyles.number}>
              {figure.figure(complete)}
            </td>
          ))}
        </tr>
      )}
      {partial !== undefined && (
        <tr>
          {identity(<Badge tone="caution">{`Partial: ${coverage(aggregate.folds)}`}</Badge>, aggregate.folds.included)}
          {TOTAL_FIGURES.map((figure) => (
            <td key={figure.label} data-label={`${figure.label}, partial`} className={historyStyles.number}>
              {figure.figure(partial)}
            </td>
          ))}
        </tr>
      )}
    </>
  );
}

function Aggregates({ study, verification }: { readonly study: Study; readonly verification: Verification }) {
  const definitions = study.definitions.aggregation;
  return (
    <Panel title="Totals over reset accounts" meta="Not a portfolio">
      <ScrollingTable label="Totals over reset accounts, scrollable">
        <table className={historyStyles.table}>
          <caption className="visually-hidden">
            Each model and scenario's results summed over its folds' independent accounts
          </caption>
          <thead>
            <tr>
              <th scope="col">Model</th>
              <th scope="col">Scenario</th>
              <th scope="col">Coverage</th>
              {TOTAL_FIGURES.map((figure) => (
                <th key={figure.label} scope="col">
                  {figure.label}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {MODELS.flatMap((name) =>
              study.scenarios.map((scenario) => {
                const aggregate = study.aggregates.trading[name][scenario.name];
                return aggregate === undefined ? null : (
                  <TotalsRows
                    key={`${name}-${scenario.name}`}
                    name={name}
                    scenario={scenario.name}
                    aggregate={aggregate}
                    verification={verification}
                  />
                );
              }),
            )}
          </tbody>
        </table>
      </ScrollingTable>
      <ul className={styles.list}>
        {['money', 'return_percent', 'complete', 'partial', 'not_computed'].map((key) => {
          const text = definitions[key];
          return text === undefined ? null : <li key={key}>{sentence(text)}</li>;
        })}
      </ul>
    </Panel>
  );
}

// --- Prediction quality, apart from trading ---------------------------------------------------------------

const CLASSES = ['down', 'flat', 'up'] as const;

// Rows predicted by a fold's schedule, or, pooled, where they are counted.
type Predicted = { readonly predicted: string; readonly rows: string } | null;

function ScoreCells({ scores, predicted }: { readonly scores: Scores; readonly predicted: Predicted }) {
  const { metrics, rows } = scores;
  const reasons = Object.entries(rows.not_scored);
  const counts = (classes: Scores['metrics']['true_distribution']) => (
    <span className="numeric">
      {classes.down} · {classes.flat} · {classes.up}
    </span>
  );
  return (
    <>
      <td data-label="Rows predicted" className={historyStyles.number}>
        {predicted === null ? (
          <Unavailable>Counted per fold</Unavailable>
        ) : (
          <span className="numeric">
            {predicted.predicted} of {predicted.rows}
          </span>
        )}
      </td>
      <td data-label="Rows scored" className={historyStyles.number}>
        <span>
          <span className="numeric">
            {rows.scored} of {rows.scheduled}
          </span>
          {reasons.map(([reason, count]) => (
            <span key={reason} className={styles.reasonCode}>
              {count} not scored: {notScoredReason(reason)}
            </span>
          ))}
        </span>
      </td>
      <td data-label="True down · flat · up" className={historyStyles.number}>
        {counts(metrics.true_distribution)}
      </td>
      <td data-label="Predicted down · flat · up" className={historyStyles.number}>
        {counts(metrics.predicted_distribution)}
      </td>
      <td data-label="Confusion: true class → predicted down · flat · up" className={historyStyles.number}>
        <span className={styles.stackedFigures}>
          {CLASSES.map((name) => (
            <span key={name} className="numeric">
              {name} → {counts(metrics.confusion_matrix[name])}
            </span>
          ))}
        </span>
      </td>
      <td data-label="Recall down · flat · up" className={historyStyles.number}>
        <span className={styles.stackedFigures}>
          {CLASSES.map((name) => (
            <MetricValue key={name} metric={metrics.per_class[name].recall} />
          ))}
        </span>
      </td>
      <td data-label="Accuracy" className={historyStyles.number}>
        <MetricValue metric={metrics.accuracy} />
      </td>
      <td data-label="Balanced accuracy" className={historyStyles.number}>
        <MetricValue metric={metrics.balanced_accuracy} />
      </td>
      <td data-label="Log loss" className={historyStyles.number}>
        <MetricValue metric={metrics.log_loss} as="number" />
      </td>
      <td data-label="Brier score" className={historyStyles.number}>
        <MetricValue metric={metrics.brier_score} as="number" />
      </td>
    </>
  );
}

const SCORE_COLUMNS = 10;

function Unscored({ reason }: { readonly reason: string }) {
  return (
    <td data-label="Scores" colSpan={SCORE_COLUMNS}>
      <Unavailable>{reason}</Unavailable>
    </td>
  );
}

function ScheduleNote({ checks }: { readonly checks: readonly (Check | undefined)[] }) {
  return checks.some((check) => check !== undefined && check.state !== 'verified') ? (
    <Badge tone="negative">Unverified schedule</Badge>
  ) : null;
}

function PredictionQuality({ study, verification }: { readonly study: Study; readonly verification: Verification }) {
  return (
    <Panel title="Prediction quality" meta="Against each recording’s realized labels">
      <ScrollingTable label="Prediction quality, scrollable">
        <table className={historyStyles.table}>
          <caption className="visually-hidden">
            Each fold and model's classification quality, then each model's scores pooled over its folds
          </caption>
          <thead>
            <tr>
              <th scope="col">Fold</th>
              <th scope="col">Model</th>
              <th scope="col">Rows predicted</th>
              <th scope="col">Rows scored</th>
              <th scope="col">True down · flat · up</th>
              <th scope="col">Predicted down · flat · up</th>
              <th scope="col">Confusion: true class → predicted down · flat · up</th>
              <th scope="col">Recall down · flat · up</th>
              <th scope="col">Accuracy</th>
              <th scope="col">Balanced accuracy</th>
              <th scope="col">Log loss</th>
              <th scope="col">Brier score</th>
            </tr>
          </thead>
          <tbody>
            {study.folds.flatMap((fold) =>
              MODELS.map((name) => {
                const model = modelOf(fold, name);
                return (
                  <tr key={`${fold.index}-${name}`}>
                    <td data-label="Fold">
                      <span>
                        {foldLabel(fold.index, fold.evaluation.stream)}
                        <ScheduleNote checks={[verification.schedules.get(`${fold.index}|${name}`)]} />
                      </span>
                    </td>
                    <td data-label="Model">{MODEL_LABEL[name]}</td>
                    {model.state !== 'completed' ? (
                      <Unscored reason={`${MODEL_STATE[model.state].label}: ${studyReason(model.reason)}`} />
                    ) : 'unavailable' in model.predictive ? (
                      <Unscored reason={`Not scored: ${studyReason(model.predictive.unavailable)}`} />
                    ) : (
                      <ScoreCells scores={model.predictive} predicted={model.schedule.counts} />
                    )}
                  </tr>
                );
              }),
            )}
            {MODELS.map((name) => {
              const pooled = study.aggregates.predictive[name];
              const label =
                pooled.status === 'complete'
                  ? 'All folds, pooled'
                  : pooled.status === 'partial'
                    ? `Pooled over ${coverage(pooled.folds)}`
                    : 'No fold scored';
              return (
                <tr key={`pooled-${name}`}>
                  <td data-label="Fold">
                    <span>
                      {label}
                      {pooled.status === 'partial' && <Badge tone="caution">Partial</Badge>}
                      <ScheduleNote checks={pooled.folds.included.map((fold) => verification.schedules.get(`${fold}|${name}`))} />
                    </span>
                  </td>
                  <td data-label="Model">{MODEL_LABEL[name]}</td>
                  {pooled.status === 'unavailable' ? (
                    <Unscored reason="No fold of this model was scored" />
                  ) : (
                    <ScoreCells scores={pooled.pooled} predicted={null} />
                  )}
                </tr>
              );
            })}
          </tbody>
        </table>
      </ScrollingTable>
      <p className={styles.note}>
        {sentence(study.definitions.aggregation.classification ?? '')} Scored by the research service after each fold’s
        predictions existed: {study.definitions.scoring.rule}. Classification quality is not trading performance.
      </p>
    </Panel>
  );
}

// --- Folds, artifacts, reproducibility ------------------------------------------------------------------

function FoldModels({ study, artifacts }: { readonly study: Study; readonly artifacts: StudyArtifacts }) {
  return (
    <Panel title="Fold models and artifacts">
      <ScrollingTable label="Fold models and artifacts, scrollable">
        <table className={historyStyles.table}>
          <caption className="visually-hidden">Each fold's training, fold models, and whether its artifacts verify</caption>
          <thead>
            <tr>
              <th scope="col">Fold</th>
              <th scope="col">Training rows · down · flat · up</th>
              <th scope="col">Latest training label known</th>
              <th scope="col">Information boundary</th>
              <th scope="col">Fold models</th>
              <th scope="col">Schedules</th>
            </tr>
          </thead>
          <tbody>
            {study.folds.map((fold) => {
              const checks = artifacts.find((entry) => entry.fold === fold.index);
              const foldModel = fold.fold_model;
              const latestLabel = fold.training?.max_label_available_at ?? null;
              return (
                <tr key={fold.index}>
                  <td data-label="Fold">{foldLabel(fold.index, fold.evaluation.stream)}</td>
                  <td data-label="Training rows · down · flat · up" className={historyStyles.number}>
                    {fold.training === undefined ? (
                      <Unavailable>Not fitted</Unavailable>
                    ) : (
                      <span className="numeric">
                        {fold.training.rows} · {fold.training.class_support.down} · {fold.training.class_support.flat} ·{' '}
                        {fold.training.class_support.up}
                      </span>
                    )}
                  </td>
                  <td data-label="Latest training label known">
                    {latestLabel === null ? <Unavailable>No label</Unavailable> : <MarketTime value={latestLabel} stacked />}
                  </td>
                  <td data-label="Information boundary">
                    <MarketTime value={fold.information_boundary} stacked />
                  </td>
                  <td data-label="Fold models">
                    {foldModel.state === 'completed' ? (
                      <span>
                        <Ids>{shortId(foldModel.fold_model_id)}</Ids> <CheckBadge check={checkOf(checks?.fold_model)} />
                      </span>
                    ) : (
                      <span>
                        <Badge tone="negative">Failed</Badge>
                        <span className={styles.reasonCode}>{studyReason(foldModel.reason)}</span>
                      </span>
                    )}
                  </td>
                  <td data-label="Schedules">
                    <span className={styles.stackedFigures}>
                      {MODELS.map((name) => {
                        const model = modelOf(fold, name);
                        const check = checks?.models[name]?.schedule;
                        return (
                          <span key={name}>
                            {MODEL_LABEL[name]}:{' '}
                            {model.state === 'completed' ? (
                              <>
                                <Ids>{shortId(model.schedule.artifact_id)}</Ids> <CheckBadge check={checkOf(check)} />
                              </>
                            ) : (
                              <>
                                <Badge tone={MODEL_STATE[model.state].tone}>{MODEL_STATE[model.state].label}</Badge>{' '}
                                {studyReason(model.reason)}
                              </>
                            )}
                          </span>
                        );
                      })}
                    </span>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </ScrollingTable>
    </Panel>
  );
}

function Reproducibility({ study }: { readonly study: Study }) {
  return (
    <Panel title="How it ran and how to reproduce it">
      <ul className={styles.list}>
        {study.definitions.execution.map((text) => (
          <li key={text}>{sentence(text)}</li>
        ))}
      </ul>
      <p className={styles.note}>
        Reproduce everything, rebuilding each fold model and schedule and replaying every run:{' '}
        <Code>{`python -m chronos_research inspect-walk-forward-study --tools <build> --artifacts ../data --study ${study.study_id}`}</Code>
      </p>
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Environment</h3>
        <Facts facts={Object.entries(study.environment).map(([label, value]) => ({ label, value }))} />
      </div>
    </Panel>
  );
}

function Summary({ study, verification }: { readonly study: Study; readonly verification: Verification }) {
  const status = STUDY_STATUS[study.status];
  const evidence = EVIDENCE[study.collection.evidence];
  const cells = Object.entries(study.outcomes.cells)
    .map(([state, count]) => `${count} ${state}`)
    .join(', ');
  return (
    <Panel
      title={`${study.instrument} · ${String(study.folds.length)} folds`}
      meta={
        <span>
          <Badge tone={status.tone}>{status.label}</Badge>{' '}
          {verification.problems.length > 0 && <Badge tone="negative">Unverified</Badge>}
        </span>
      }
    >
      <Facts
        facts={[
          { label: 'Study ID', value: <Ids>{study.study_id}</Ids> },
          { label: 'Plan', value: <a href={walkForwardHref({ planId: study.plan_id })}>{shortId(study.plan_id)}</a> },
          {
            label: 'Collection',
            value: (
              <>
                <Ids>{shortId(study.collection.collection_id)}</Ids> <Badge tone={evidence.tone}>{evidence.label}</Badge>
              </>
            ),
          },
          { label: 'Instrument', value: study.instrument },
          { label: 'Engine runs', value: cells },
          {
            label: 'Verification, read again now',
            value:
              verification.problems.length === 0
                ? 'Every fold model, schedule, and run verifies'
                : verification.problems.length === 1
                  ? '1 artifact does not verify or was not checked'
                  : `${String(verification.problems.length)} artifacts do not verify or were not checked`,
          },
          {
            label: 'Scenarios',
            value: study.scenarios.map((cell) => `${scenarioLabel(cell.name)} (${cell.fee_per_fill} USD, ${cell.latency_ns} ns)`).join('; '),
          },
        ]}
      />
      <p className={styles.note}>{sentence(study.scope)}</p>
    </Panel>
  );
}

export function StudyDetailView({ studyId }: { readonly studyId: string }) {
  const detail = useStudy(studyId);
  if (detail.status === 'pending') {
    return (
      <Panel title="Study">
        <p role="status">Reading and verifying the study…</p>
      </Panel>
    );
  }
  if (detail.status === 'error') {
    return (
      <Panel title="Study">
        <ResearchFailure
          error={detail.error}
          title="The study could not be read"
          onRetry={() => void detail.refetch()}
          retrying={detail.isFetching}
        />
      </Panel>
    );
  }
  const { study, artifacts } = detail.data;
  const verification = studyVerification(study, artifacts);
  return (
    <>
      {verification.problems.length > 0 && (
        <Alert tone="negative" title="Some of this study’s artifacts do not verify">
          <p>
            Read again now, these no longer match what the study recorded. Figures that rest on them are shown as
            recorded and labelled unverified; they are not confirmed. Reproduce the study with the command below.
          </p>
          <ul className={styles.list}>
            {verification.problems.map((text) => (
              <li key={text}>{text}</li>
            ))}
          </ul>
        </Alert>
      )}
      {study.collection.evidence === 'synthetic' && <SyntheticNotice />}
      {study.status === 'partial' && (
        <Alert tone="caution" title="This study is partial">
          <p>Some planned folds, models, or runs did not complete. Nothing missing is counted as zero:</p>
          <ul className={styles.list}>
            {study.missing.map((entry) => (
              <li key={`${entry.fold}-${entry.item}`}>
                Fold {entry.fold}, {missingItem(entry.item)}: {studyReason(entry.reason)}{' '}
                <span className={styles.reasonCode}>{entry.reason}</span>
              </li>
            ))}
          </ul>
        </Alert>
      )}
      <Alert tone="info" title="A few recordings cannot establish skill">
        <p>
          These are the results of fixed models on these recordings, fold by fold. There is no significance test, no
          annualized ratio, no combined equity curve, and no winner.
        </p>
      </Alert>
      <Summary study={study} verification={verification} />
      <TradingByFold study={study} verification={verification} />
      <Aggregates study={study} verification={verification} />
      <PredictionQuality study={study} verification={verification} />
      <FoldModels study={study} artifacts={artifacts} />
      <Reproducibility study={study} />
    </>
  );
}
