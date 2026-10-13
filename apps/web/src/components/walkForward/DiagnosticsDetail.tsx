import type { ReactNode } from 'react';
import { useId, useState } from 'react';

import type {
  AcrossPool,
  Cohort,
  CompletedRun,
  Components,
  DecisionCohorts,
  DiagnosticAccount,
  DiagnosticFold,
  DiagnosticRun,
  Diagnostics,
  DiagnosticsVerification,
  Figure,
  FigureTrust,
  Ratio,
  Returns,
  SimulatedAccount,
  StudySection,
  TradingPool,
  Trust,
} from '../../api/diagnosticsSchemas';
import { SIMULATED_ACCOUNTS } from '../../api/diagnosticsSchemas';
import type { ModelName } from '../../api/researchSchemas';
import {
  ACCOUNT_LABEL,
  BRIDGE,
  COHORT_LABEL,
  COMPONENT_LABEL,
  OVERVIEW_COMPONENTS,
  POOL_STATUS,
  RECONCILIATION,
  RECORDED_RECONCILIATION,
  SIGNAL_BRIDGE,
  SOURCE_LABEL,
  diagnosticReason,
  sourceCounts,
  sourceProblems,
  trusted,
  untrustedSources,
  verificationLevels,
} from '../../presentation/diagnostics';
import { sentence } from '../../presentation/models';
import { MODEL_LABEL, shortId } from '../../presentation/research';
import { EVIDENCE, scenarioLabel } from '../../presentation/walkForward';
import { diagnosticsHref, researchHref, walkForwardHref } from '../../state/route';
import { useDiagnosticEpisodes, useDiagnosticFills, useDiagnostics } from '../../state/diagnostics';
import historyStyles from '../history/history.module.css';
import { ListFooter } from '../research/ListFooter';
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

/** An exact figure, or why it does not exist, with its code. */
export function FigureText({ figure, signed = false }: { readonly figure: Figure; readonly signed?: boolean }) {
  if ('unavailable' in figure) {
    return (
      <span>
        <Unavailable>{`Unavailable: ${diagnosticReason(figure.unavailable)}`}</Unavailable>
        <span className={styles.reasonCode}>{figure.unavailable}</span>
      </span>
    );
  }
  return <DecimalText value={figure.value} signed={signed} />;
}

function RatioText({ ratio }: { readonly ratio: Ratio }) {
  if ('undefined' in ratio) {
    return <Unavailable>{`Undefined: ${diagnosticReason(ratio.undefined)}`}</Unavailable>;
  }
  return (
    <span className={styles.stackedFigures}>
      <span className="numeric">{ratio.rounded}</span>
      <span className={styles.reasonCode}>
        {ratio.numerator} / {ratio.denominator}
      </span>
    </span>
  );
}

function Durations({ value }: { readonly value: DiagnosticAccount['timing']['signal_to_execution_duration_ns'] }) {
  if ('unavailable' in value) {
    return <Unavailable>{diagnosticReason(value.unavailable)}</Unavailable>;
  }
  return (
    <span className="numeric">
      {value.minimum} to {value.maximum} ns, {value.sum} ns in all
    </span>
  );
}

function runOf(fold: DiagnosticFold, model: ModelName, scenario: string): DiagnosticRun | undefined {
  return fold.runs.find((run) => run.model === model && run.scenario === scenario);
}

interface Located {
  readonly study: StudySection;
  readonly fold: DiagnosticFold;
  readonly run: CompletedRun;
}

type Selected = Located & { readonly diagnosticId: string };

function locate(diagnostics: Diagnostics, runId: string): Located | null {
  for (const study of diagnostics.studies) {
    for (const fold of study.folds) {
      for (const run of fold.runs) {
        if (run.state === 'completed' && run.run_id === runId) {
          return { study, fold, run };
        }
      }
    }
  }
  return null;
}

type PoolTrust = NonNullable<FigureTrust['across_studies']>;

function runTrust(verification: DiagnosticsVerification, runId: string): Trust | undefined {
  return verification.sources.now.figures.runs[runId];
}

/** Said, after what it labels, of figures whose sources did not all verify
 * on this read; nothing is said of figures whose sources did. */
function TrustBadge({ trust, noun }: { readonly trust: Trust | undefined; readonly noun: string }) {
  return trusted(trust) ? null : (
    <>
      {' '}
      <Badge tone="negative">{`Unverified ${noun}`}</Badge>
    </>
  );
}

function SectionTitle({ children, trust }: { readonly children: string; readonly trust: Trust | undefined }) {
  return (
    <h3 className={styles.sectionTitle}>
      {children}
      {!trusted(trust) && (
        <>
          {' '}
          <Badge tone="negative">Recorded, unverified</Badge>
        </>
      )}
    </h3>
  );
}

// --- Summary and verification ---------------------------------------------------------------------------

function Summary({ diagnostics }: { readonly diagnostics: Diagnostics }) {
  const status = diagnostics.status === 'complete' ? POOL_STATUS.complete : POOL_STATUS.partial;
  return (
    <Panel
      title={diagnostics.studies.map((study) => study.instrument).join(' · ')}
      meta={<Badge tone={status.tone}>{diagnostics.status === 'complete' ? 'Complete' : 'Partial'}</Badge>}
    >
      <Facts
        facts={[
          { label: 'Diagnostics ID', value: <Ids>{diagnostics.diagnostic_id}</Ids> },
          {
            label: 'Studies',
            value: (
              <span className={styles.stackedFigures}>
                {diagnostics.studies.map((study) => (
                  <span key={study.study_id}>
                    <a href={walkForwardHref({ studyId: study.study_id })}>
                      {study.instrument} study {shortId(study.study_id)}
                    </a>{' '}
                    <Badge tone={EVIDENCE[study.evidence].tone}>{EVIDENCE[study.evidence].label}</Badge>
                  </span>
                ))}
              </span>
            ),
          },
          {
            label: 'Runs attributed',
            value: `${diagnostics.counts.runs_attributed} of ${diagnostics.counts.runs}`,
          },
          { label: 'Fills · holding episodes', value: `${diagnostics.counts.fills} · ${diagnostics.counts.episodes}` },
        ]}
      />
      <p className={styles.note}>{sentence(diagnostics.scope)}</p>
    </Panel>
  );
}

function VerificationPanel({
  diagnostics,
  verification,
}: {
  readonly diagnostics: Diagnostics;
  readonly verification: DiagnosticsVerification;
}) {
  const levels = verificationLevels(verification);
  return (
    <Panel title="Verification, level by level" meta="No level stands for another">
      <ScrollingTable label="Verification levels, scrollable">
        <table className={historyStyles.table}>
          <caption className="visually-hidden">Each level of verification and its state</caption>
          <thead>
            <tr>
              <th scope="col">Level</th>
              <th scope="col">State</th>
              <th scope="col">What it covers</th>
            </tr>
          </thead>
          <tbody>
            {levels.map((level) => (
              <tr key={level.name}>
                <td data-label="Level">
                  {level.name}
                </td>
                <td data-label="State">
                  <Badge tone={level.state.tone}>{level.state.label}</Badge>
                </td>
                <td data-label="What it covers" className={historyStyles.detail}>
                  <span className={styles.stackedFigures}>
                    <span>{sentence(level.detail)}</span>
                    {level.command !== undefined && (
                      <Code>{level.command.replaceAll('<diagnostic_id>', diagnostics.diagnostic_id)}</Code>
                    )}
                  </span>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </ScrollingTable>
      <Facts
        facts={sourceCounts(verification).map(({ kind, checked, verified }) => ({
          label: SOURCE_LABEL[kind].many,
          value:
            checked === 0 ? (
              'None referenced'
            ) : verified === checked ? (
              `${String(verified)} of ${String(checked)} verified`
            ) : (
              <span>
                <Badge tone="negative">{`${String(checked - verified)} not verified`}</Badge>{' '}
                <span className="numeric">{`${String(verified)} of ${String(checked)} verified`}</span>
              </span>
            ),
        }))}
      />
    </Panel>
  );
}

// --- One run --------------------------------------------------------------------------------------------

function RunSelector({ located, account }: { readonly located: Selected; readonly account: SimulatedAccount }) {
  const formId = useId();
  const { study, fold, run } = located;
  const [missing, setMissing] = useState<string | null>(null);
  const go = (index: string, model: ModelName, scenario: string) => {
    const target = study.folds.find((item) => item.index === index);
    const chosen = target === undefined ? undefined : runOf(target, model, scenario);
    if (chosen?.state !== 'completed') {
      const reason = chosen?.state === 'unavailable' ? diagnosticReason(chosen.reason) : 'not planned';
      setMissing(`Fold ${index}, ${MODEL_LABEL[model]}, ${scenarioLabel(scenario)} has no run: ${reason}`);
      return;
    }
    setMissing(null);
    window.location.hash = diagnosticsHref({ diagnosticId: located.diagnosticId, runId: chosen.run_id, account });
  };
  return (
    <div className={styles.section}>
      <div className={styles.fields}>
        <div className={styles.field}>
          <label htmlFor={`${formId}-fold`}>Fold</label>
          <select
            id={`${formId}-fold`}
            value={fold.index}
            onChange={(event) => {
              go(event.target.value, run.model, run.scenario);
            }}
          >
            {study.folds.map((item) => (
              <option key={item.index} value={item.index}>
                {`Fold ${item.index} · ${item.stream}`}
              </option>
            ))}
          </select>
        </div>
        <div className={styles.field}>
          <label htmlFor={`${formId}-model`}>Model</label>
          <select
            id={`${formId}-model`}
            value={run.model}
            onChange={(event) => {
              go(fold.index, event.target.value === 'majority' ? 'majority' : 'logistic', run.scenario);
            }}
          >
            {MODELS.map((name) => (
              <option key={name} value={name}>
                {MODEL_LABEL[name]}
              </option>
            ))}
          </select>
        </div>
        <div className={styles.field}>
          <label htmlFor={`${formId}-scenario`}>Scenario</label>
          <select
            id={`${formId}-scenario`}
            value={run.scenario}
            onChange={(event) => {
              go(fold.index, run.model, event.target.value);
            }}
          >
            {study.scenarios.map((scenario) => (
              <option key={scenario.name} value={scenario.name}>
                {`${scenarioLabel(scenario.name)} (${scenario.fee_per_fill} USD, ${scenario.latency_ns} ns)`}
              </option>
            ))}
          </select>
        </div>
      </div>
      {missing !== null && (
        <p className={styles.fieldError} role="status">
          {missing}
        </p>
      )}
      <nav aria-label="Simulated account" className={styles.inline}>
        {SIMULATED_ACCOUNTS.map((name) => (
          <a
            key={name}
            className={styles.back}
            href={diagnosticsHref({ diagnosticId: located.diagnosticId, runId: run.run_id, account: name })}
            aria-current={name === account ? 'page' : undefined}
          >
            {name === account ? `${ACCOUNT_LABEL[name]} (shown)` : ACCOUNT_LABEL[name]}
          </a>
        ))}
      </nav>
      <p className={styles.note}>
        These are the run’s own simulated accounts, each started with its own cash. Your paper-trading account is not
        involved and stays selected.
      </p>
    </div>
  );
}

function Bridge({
  components,
  rows,
  caption,
  definitions,
}: {
  readonly components: Components;
  readonly rows: typeof BRIDGE;
  readonly caption: string;
  readonly definitions: Diagnostics['definitions']['attribution'];
}) {
  return (
    <ScrollingTable label={`${caption}, scrollable`}>
      <table className={historyStyles.table}>
        <caption className="visually-hidden">{caption}</caption>
        <thead>
          <tr>
            <th scope="col">Term</th>
            <th scope="col">USD, exact</th>
            <th scope="col">Definition</th>
          </tr>
        </thead>
        <tbody>
          {rows.map(({ component, operator }) => (
            <tr key={component}>
              <td data-label="Term">
                {operator === '' ? COMPONENT_LABEL[component] : `${operator} ${COMPONENT_LABEL[component]}`}
              </td>
              <td data-label={COMPONENT_LABEL[component]} className={historyStyles.number}>
                <FigureText figure={components[component]} signed={component.endsWith('pnl')} />
              </td>
              <td data-label="Definition" className={historyStyles.detail}>
                <Code>{definitions[component] ?? component}</Code>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </ScrollingTable>
  );
}

function ReconciliationBadges({ account, current }: { readonly account: DiagnosticAccount; readonly current: boolean }) {
  const booked = account.reconciliation.booked_net_pnl;
  const signal = account.reconciliation.signal_reference;
  // An identity that held when generated is current only if its sources are.
  const words = current ? RECONCILIATION : RECORDED_RECONCILIATION;
  return (
    <Facts
      facts={[
        {
          label: 'Net PnL identity',
          value:
            'state' in booked ? (
              <Badge tone={words[booked.state].tone}>{words[booked.state].label}</Badge>
            ) : (
              <span>
                <Badge tone="caution">Not checkable</Badge>{' '}
                <span className={styles.reasonCode}>{diagnosticReason(booked.unavailable)}</span>
              </span>
            ),
        },
        {
          label: 'Signal-reference identity',
          value:
            'state' in signal ? (
              <Badge tone={words.reconciled.tone}>{words.reconciled.label}</Badge>
            ) : (
              <span>
                <Badge tone="caution">Not checkable</Badge>{' '}
                <span className={styles.reasonCode}>{diagnosticReason(signal.unavailable)}</span>
              </span>
            ),
        },
      ]}
    />
  );
}

function AccountFacts({ account, run }: { readonly account: DiagnosticAccount; readonly run: CompletedRun }) {
  const valuation = account.booked.valuation;
  const mark = account.final_mark;
  const outcomes = account.orders.outcomes;
  const spreads = account.quoted_spreads;
  const spread = (value: typeof spreads.execution) =>
    'unavailable' in value ? (
      <Unavailable>{diagnosticReason(value.unavailable)}</Unavailable>
    ) : (
      <span className="numeric">
        <DecimalText value={value.minimum} /> to <DecimalText value={value.maximum} /> over {value.fills} fills
      </span>
    );
  return (
    <Facts
      facts={[
        {
          label: 'Booked valuation',
          value:
            'unavailable' in valuation ? (
              <Unavailable>{`Unavailable: ${diagnosticReason(valuation.unavailable)}`}</Unavailable>
            ) : (
              <span className="numeric">
                Equity <DecimalText value={valuation.equity} />, return{' '}
                <DecimalText value={valuation.return_percent} signed suffix="%" />
              </span>
            ),
        },
        { label: 'Final position (shares)', value: <DecimalText value={account.booked.final_quantity} /> },
        {
          label: 'Final mark',
          value:
            mark === null ? (
              'Flat: no mark needed'
            ) : 'unavailable' in mark ? (
              <Unavailable>{`Unavailable: ${diagnosticReason(mark.unavailable)}`}</Unavailable>
            ) : (
              <span className={styles.stackedFigures}>
                <span className="numeric">
                  Bid <DecimalText value={mark.bid} /> · ask{' '}
                  {mark.ask === null ? <Unavailable>none</Unavailable> : <DecimalText value={mark.ask} />} · midpoint{' '}
                  <FigureText figure={mark.midpoint} />
                </span>
                <MarketTime value={mark.receive_at} />
              </span>
            ),
        },
        {
          label: 'Orders scheduled · filled · partly filled · cancelled · rejected · expired',
          value: (
            <span className="numeric">
              {outcomes.scheduled} · {outcomes.filled} · {outcomes.partially_filled_and_cancelled} · {outcomes.cancelled} ·{' '}
              {outcomes.rejected} · {outcomes.expired}
            </span>
          ),
        },
        {
          label: 'Rejections and cancellations',
          value:
            Object.keys(account.orders.rejections).length + Object.keys(account.orders.cancellation_reasons).length ===
            0 ? (
              'None'
            ) : (
              <span className={styles.stackedFigures}>
                {Object.entries({ ...account.orders.rejections, ...account.orders.cancellation_reasons }).map(
                  ([reason, count]) => (
                    <span key={reason}>
                      {count} {diagnosticReason(reason)}
                    </span>
                  ),
                )}
              </span>
            ),
        },
        { label: 'Configured latency', value: <span className="numeric">{account.timing.configured_latency_ns} ns</span> },
        { label: 'Observed: signal to arrival', value: <Durations value={account.timing.signal_to_arrival_duration_ns} /> },
        {
          label: 'Observed: arrival to execution',
          value: <Durations value={account.timing.arrival_to_execution_duration_ns} />,
        },
        {
          label: 'Executions · expired orders',
          value: `${account.timing.executions} · ${account.timing.expired}`,
        },
        { label: 'Quoted spread at execution', value: spread(spreads.execution) },
        { label: 'Quoted spread at the signal', value: spread(spreads.signal) },
        {
          label: 'Holding episodes completed · censored at the end',
          value: `${account.episodes.completed} · ${account.episodes.censored}`,
        },
        {
          label: 'Completed episodes’ booked PnL',
          value: (
            <span className="numeric">
              <FigureText figure={account.episodes.completed_pnl} signed /> ({account.episodes.completed_with_gain} gained,{' '}
              {account.episodes.completed_with_loss} lost)
            </span>
          ),
        },
        { label: 'Engine replay', value: run.engine_replay.statement ?? 'Not recorded by the study' },
      ]}
    />
  );
}

function Deltas({ run }: { readonly run: CompletedRun }) {
  return (
    <ScrollingTable label="Strategy minus each benchmark, scrollable">
      <table className={historyStyles.table}>
        <caption className="visually-hidden">The strategy minus buy and hold and minus cash, term by term</caption>
        <thead>
          <tr>
            <th scope="col">Term</th>
            <th scope="col">Strategy − buy and hold</th>
            <th scope="col">Strategy − cash</th>
          </tr>
        </thead>
        <tbody>
          {OVERVIEW_COMPONENTS.map((component) => (
            <tr key={component}>
              <td data-label="Term">
                {COMPONENT_LABEL[component]}
              </td>
              <td data-label="Strategy − buy and hold" className={historyStyles.number}>
                <FigureText figure={run.deltas.buy_and_hold[component]} signed={component.endsWith('pnl')} />
              </td>
              <td data-label="Strategy − cash" className={historyStyles.number}>
                <FigureText figure={run.deltas.cash[component]} signed={component.endsWith('pnl')} />
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </ScrollingTable>
  );
}

function counts(classes: Cohort['predicted']): string {
  return `${classes.down} · ${classes.flat} · ${classes.up}`;
}

interface CohortEntry {
  readonly name: string;
  readonly cohort: Cohort;
  readonly trust: Trust | undefined;
}

function CohortsTable({ cohorts, caption }: { readonly cohorts: readonly CohortEntry[]; readonly caption: string }) {
  return (
    <ScrollingTable label={`${caption}, scrollable`}>
      <table className={historyStyles.table}>
        <caption className="visually-hidden">{caption}</caption>
        <thead>
          <tr>
            <th scope="col">Cohort</th>
            <th scope="col">Items</th>
            <th scope="col">Scored</th>
            <th scope="col">Not scored, by label status</th>
            <th scope="col">Predicted down · flat · up</th>
            <th scope="col">Realized down · flat · up</th>
            <th scope="col">Accuracy</th>
            <th scope="col">Balanced accuracy</th>
            <th scope="col">Reasons or outcomes</th>
          </tr>
        </thead>
        <tbody>
          {cohorts.map(({ name, cohort, trust }) => (
            <tr key={name}>
              <td data-label="Cohort">
                {name}
                <TrustBadge trust={trust} noun="cohort" />
              </td>
              <td data-label="Items" className={historyStyles.number}>
                {cohort.items}
              </td>
              <td data-label="Scored" className={historyStyles.number}>
                {cohort.scored}
              </td>
              <td data-label="Not scored, by label status" className={historyStyles.detail}>
                <span className={styles.stackedFigures}>
                  {Object.entries(cohort.labels)
                    .filter(([status]) => status !== 'scored')
                    .map(([status, count]) => (
                      <span key={status}>
                        {count} {diagnosticReason(status)}
                      </span>
                    ))}
                </span>
              </td>
              <td data-label="Predicted down · flat · up" className={historyStyles.number}>
                {counts(cohort.predicted)}
              </td>
              <td data-label="Realized down · flat · up" className={historyStyles.number}>
                {counts(cohort.realized)}
              </td>
              <td data-label="Accuracy" className={historyStyles.number}>
                <RatioText ratio={cohort.accuracy} />
              </td>
              <td data-label="Balanced accuracy" className={historyStyles.number}>
                <RatioText ratio={cohort.balanced_accuracy} />
              </td>
              <td data-label="Reasons or outcomes" className={historyStyles.detail}>
                <span className={styles.stackedFigures}>
                  {Object.entries(cohort.reasons).map(([reason, count]) => (
                    <span key={reason}>
                      {count} {diagnosticReason(reason)}
                    </span>
                  ))}
                </span>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </ScrollingTable>
  );
}

function decisionCohorts(
  scorable: { readonly cohort: Cohort; readonly trust: Trust | undefined } | null,
  decisions: DecisionCohorts,
  trust: Trust | undefined,
): readonly CohortEntry[] {
  return [
    ...(scorable === null ? [] : [{ name: COHORT_LABEL.scorable_predictions, ...scorable }]),
    { name: COHORT_LABEL.sampled_decisions, cohort: decisions.sampled_decisions, trust },
    { name: COHORT_LABEL.order_scheduling_decisions, cohort: decisions.order_scheduling_decisions, trust },
    { name: COHORT_LABEL.filled_orders, cohort: decisions.filled_orders, trust },
  ];
}

/** A mean future return in the label's unit, with its exact parts. */
function MeanReturn({ returns }: { readonly returns: Returns }) {
  const mean = returns.mean_raw;
  if ('undefined' in mean) {
    return <Unavailable>{`Undefined: ${diagnosticReason(mean.undefined)}`}</Unavailable>;
  }
  return (
    <span className={styles.stackedFigures}>
      <span className="numeric">{mean.rounded} ppb</span>
      <span className={styles.reasonCode}>
        {returns.sum_raw} / {returns.rows} rows
      </span>
    </span>
  );
}

function Extreme({ value }: { readonly value: Returns['minimum_raw'] }) {
  return 'undefined' in value ? (
    <Unavailable>{`Undefined: ${diagnosticReason(value.undefined)}`}</Unavailable>
  ) : (
    <span className="numeric">{value.value} ppb</span>
  );
}

const PREDICTED: readonly { readonly name: 'down' | 'flat' | 'up'; readonly label: string }[] = [
  { name: 'down', label: 'After a down prediction' },
  { name: 'flat', label: 'After a flat prediction' },
  { name: 'up', label: 'After an up prediction' },
];

function FutureReturns({ cohorts, caption }: { readonly cohorts: readonly CohortEntry[]; readonly caption: string }) {
  return (
    <ScrollingTable label={`${caption}, scrollable`}>
      <table className={historyStyles.table}>
        <caption className="visually-hidden">{caption}</caption>
        <thead>
          <tr>
            <th scope="col">Cohort</th>
            <th scope="col">Scored · not scored</th>
            <th scope="col">Mean</th>
            <th scope="col">Minimum</th>
            <th scope="col">Maximum</th>
            {PREDICTED.map(({ label }) => (
              <th key={label} scope="col">
                {label}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {cohorts.map(({ name, cohort, trust }) => {
            const returns = cohort.future_returns;
            return (
              <tr key={name}>
                <td data-label="Cohort">
                  {name}
                  <TrustBadge trust={trust} noun="cohort" />
                </td>
                <td data-label="Scored · not scored" className={historyStyles.number}>
                  {returns.all.rows} · {returns.not_scored}
                </td>
                <td data-label="Mean" className={historyStyles.number}>
                  <MeanReturn returns={returns.all} />
                </td>
                <td data-label="Minimum" className={historyStyles.number}>
                  <Extreme value={returns.all.minimum_raw} />
                </td>
                <td data-label="Maximum" className={historyStyles.number}>
                  <Extreme value={returns.all.maximum_raw} />
                </td>
                {PREDICTED.map(({ name: predicted, label }) => (
                  <td key={predicted} data-label={label} className={historyStyles.number}>
                    <MeanReturn returns={returns.by_predicted_class[predicted]} />
                  </td>
                ))}
              </tr>
            );
          })}
        </tbody>
      </table>
    </ScrollingTable>
  );
}

function Bins({ cohorts }: { readonly cohorts: readonly CohortEntry[] }) {
  const first = cohorts[0];
  if (first === undefined) {
    return null;
  }
  return (
    <ScrollingTable label="Confidence bins, scrollable">
      <table className={historyStyles.table}>
        <caption className="visually-hidden">Fixed confidence bins: items, scored items, and accuracy per cohort</caption>
        <thead>
          <tr>
            <th scope="col">Confidence</th>
            {cohorts.map(({ name }) => (
              <th key={name} scope="col">
                {name}: items · scored · accuracy · mean future return
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {first.cohort.confidence_bins.map((bin, index) => (
            <tr key={bin.name}>
              <td data-label="Confidence">
                {bin.name}
              </td>
              {cohorts.map(({ name, cohort }) => {
                const own = cohort.confidence_bins[index];
                const returns = cohort.future_returns.by_confidence_bin[index];
                return (
                  <td
                    key={name}
                    data-label={`${name}: items · scored · accuracy · mean future return`}
                    className={historyStyles.number}
                  >
                    {own === undefined || returns === undefined ? (
                      <Unavailable>Unavailable</Unavailable>
                    ) : (
                      <span className={styles.stackedFigures}>
                        <span className="numeric">
                          {own.items} · {own.scored}
                        </span>
                        <RatioText ratio={own.accuracy} />
                        <MeanReturn returns={returns} />
                      </span>
                    )}
                  </td>
                );
              })}
            </tr>
          ))}
        </tbody>
      </table>
    </ScrollingTable>
  );
}

function Fills({
  diagnosticId,
  runId,
  account,
}: {
  readonly diagnosticId: string;
  readonly runId: string;
  readonly account: SimulatedAccount;
}) {
  const fills = useDiagnosticFills(diagnosticId, runId, account);
  if (fills.status === 'pending') {
    return <p role="status">Loading this account’s fills…</p>;
  }
  if (fills.data === undefined) {
    return (
      <ResearchFailure
        error={fills.error}
        title="The fills could not be read"
        onRetry={() => void fills.refetch()}
        retrying={fills.isFetching}
      />
    );
  }
  if (fills.data.fills.length === 0) {
    return (
      <div className={historyStyles.empty}>
        <p>This account has no fill, so every execution term is a genuine zero.</p>
      </div>
    );
  }
  return (
    <>
      <ScrollingTable label="Fills, scrollable">
        <table className={historyStyles.table}>
          <caption className="visually-hidden">Every fill of this account, with its quotes and exact terms</caption>
          <thead>
            <tr>
              <th scope="col">Fill</th>
              <th scope="col">Side · quantity · price</th>
              <th scope="col">Execution quote bid · ask</th>
              <th scope="col">Execution midpoint</th>
              <th scope="col">Signal midpoint</th>
              <th scope="col">Spread paid</th>
              <th scope="col">Midpoint move</th>
              <th scope="col">Quoted spread, execution · signal</th>
              <th scope="col">Signal to execution</th>
              <th scope="col">Outcome</th>
            </tr>
          </thead>
          <tbody>
            {fills.data.fills.map((fill) => (
              <tr key={fill.fill_id}>
                <td data-label="Fill">
                  <span className={styles.stackedFigures}>
                    <span className="mono">{fill.client_order_id}</span>
                    <MarketTime value={fill.execution.executed_at} />
                  </span>
                </td>
                <td data-label="Side · quantity · price" className={historyStyles.number}>
                  <span className="numeric">
                    {fill.side} <DecimalText value={fill.quantity} /> @ <DecimalText value={fill.price} />
                  </span>
                </td>
                <td data-label="Execution quote bid · ask" className={historyStyles.number}>
                  <span className="numeric">
                    {fill.execution.bid === null ? <Unavailable>none</Unavailable> : <DecimalText value={fill.execution.bid.price} />} ·{' '}
                    {fill.execution.ask === null ? <Unavailable>none</Unavailable> : <DecimalText value={fill.execution.ask.price} />}
                  </span>
                </td>
                <td data-label="Execution midpoint" className={historyStyles.number}>
                  <FigureText figure={fill.execution.midpoint} />
                </td>
                <td data-label="Signal midpoint" className={historyStyles.number}>
                  <FigureText figure={fill.signal.midpoint} />
                </td>
                <td data-label="Spread paid" className={historyStyles.number}>
                  <FigureText figure={fill.contributions.execution_spread_cost} />
                </td>
                <td data-label="Midpoint move" className={historyStyles.number}>
                  <FigureText figure={fill.contributions.observed_midpoint_movement} />
                </td>
                <td data-label="Quoted spread, execution · signal" className={historyStyles.number}>
                  <span className={styles.stackedFigures}>
                    <FigureText figure={fill.execution.quoted_spread} />
                    <FigureText figure={fill.signal.quoted_spread} />
                  </span>
                </td>
                <td data-label="Signal to execution" className={historyStyles.number}>
                  <span className="numeric">{fill.timing.signal_to_execution_duration_ns} ns</span>
                </td>
                <td data-label="Outcome">{diagnosticReason(fill.outcome)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </ScrollingTable>
      <ListFooter
        shown={fills.data.fills.length}
        noun="fills"
        hasMore={fills.hasNextPage}
        loading={fills.isFetchingNextPage}
        failed={fills.isFetchNextPageError}
        onLoadMore={() => void fills.fetchNextPage()}
      />
    </>
  );
}

function Episodes({
  diagnosticId,
  runId,
  account,
}: {
  readonly diagnosticId: string;
  readonly runId: string;
  readonly account: SimulatedAccount;
}) {
  const episodes = useDiagnosticEpisodes(diagnosticId, runId, account);
  if (episodes.status === 'pending') {
    return <p role="status">Loading this account’s holding episodes…</p>;
  }
  if (episodes.data === undefined) {
    return (
      <ResearchFailure
        error={episodes.error}
        title="The holding episodes could not be read"
        onRetry={() => void episodes.refetch()}
        retrying={episodes.isFetching}
      />
    );
  }
  if (episodes.data.episodes.length === 0) {
    return (
      <div className={historyStyles.empty}>
        <p>This account never held a position.</p>
      </div>
    );
  }
  return (
    <>
      <ScrollingTable label="Holding episodes, scrollable">
        <table className={historyStyles.table}>
          <caption className="visually-hidden">Every holding episode of this account</caption>
          <thead>
            <tr>
              <th scope="col">Episode</th>
              <th scope="col">State</th>
              <th scope="col">Entered</th>
              <th scope="col">Exited or censored</th>
              <th scope="col">Duration</th>
              <th scope="col">Fills · bought · sold</th>
              <th scope="col">Spread paid</th>
              <th scope="col">Fees</th>
              <th scope="col">PnL</th>
            </tr>
          </thead>
          <tbody>
            {episodes.data.episodes.map((episode) => (
              <tr key={episode.episode}>
                <td data-label="Episode" className={historyStyles.number}>
                  {episode.episode}
                </td>
                <td data-label="State">
                  <Badge tone={episode.state === 'completed' ? 'neutral' : 'caution'}>
                    {episode.state === 'completed' ? 'Completed' : 'Censored at the window’s end'}
                  </Badge>
                </td>
                <td data-label="Entered">
                  <MarketTime value={episode.entry_at} stacked />
                </td>
                <td data-label="Exited or censored">
                  <MarketTime value={episode.exit_at ?? episode.censored_at ?? episode.entry_at} stacked />
                </td>
                <td data-label="Duration" className={historyStyles.number}>
                  {episode.duration_ns} ns
                </td>
                <td data-label="Fills · bought · sold" className={historyStyles.number}>
                  <span className="numeric">
                    {episode.fills} · <DecimalText value={episode.bought} /> · <DecimalText value={episode.sold} />
                  </span>
                </td>
                <td data-label="Spread paid" className={historyStyles.number}>
                  <FigureText figure={episode.execution_spread_cost} />
                </td>
                <td data-label="Fees" className={historyStyles.number}>
                  <FigureText figure={episode.fees} />
                </td>
                <td data-label="PnL" className={historyStyles.number}>
                  <span className={styles.stackedFigures}>
                    <FigureText figure={episode.pnl} signed />
                    <span className={styles.reasonCode}>
                      {episode.basis === 'booked' ? 'booked' : 'marked at the final valuation'}
                    </span>
                  </span>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </ScrollingTable>
      <ListFooter
        shown={episodes.data.episodes.length}
        noun="episodes"
        hasMore={episodes.hasNextPage}
        loading={episodes.isFetchingNextPage}
        failed={episodes.isFetchNextPageError}
        onLoadMore={() => void episodes.fetchNextPage()}
      />
    </>
  );
}

function RunDetail({
  located,
  account,
  diagnostics,
  verification,
}: {
  readonly located: Selected;
  readonly account: SimulatedAccount;
  readonly diagnostics: Diagnostics;
  readonly verification: DiagnosticsVerification;
}) {
  const { study, fold, run } = located;
  const document = run.accounts[account];
  const model = fold.models[run.model];
  const trust = runTrust(verification, run.run_id);
  const current = trusted(trust);
  const predictionTrust = verification.sources.now.figures.studies[study.study_id]?.folds[fold.index]?.[run.model];
  const scorable = model.state === 'completed' ? { cohort: model.predictions.cohort, trust: predictionTrust } : null;
  const cohorts = decisionCohorts(scorable, run.decisions.cohorts, trust);
  return (
    <Panel
      title={`${study.instrument} fold ${fold.index} · ${MODEL_LABEL[run.model]} · ${scenarioLabel(run.scenario)} · ${ACCOUNT_LABEL[account]}`}
      meta={current ? <Badge tone="neutral">Its sources match on this read</Badge> : <Badge tone="negative">Unverified run</Badge>}
      id="selected-run"
    >
      <RunSelector located={located} account={account} />
      {!current && (
        <Alert tone="negative" title="Some sources of this run no longer match what the diagnostics read">
          <p>
            Its attribution, identities, differences from its benchmarks, decision cohorts, episodes, and fills are
            shown as recorded when the diagnostics were generated, and are not confirmed now. Generate the diagnostics
            again once these are restored:
          </p>
          <ul className={styles.list}>
            {untrustedSources(trust).map((text) => (
              <li key={text}>{text}</li>
            ))}
          </ul>
        </Alert>
      )}
      <Facts
        facts={[
          { label: 'Run', value: <a href={researchHref(run.run_id)}>Open run {shortId(run.run_id)}</a> },
          { label: 'Evaluation recording', value: <Ids>{fold.stream}</Ids> },
          { label: 'Fee per fill · latency', value: `${run.fee_per_fill} USD · ${run.latency_ns} ns` },
          { label: 'Fills of this account', value: document.fills },
        ]}
      />
      <div className={styles.section}>
        <SectionTitle trust={trust}>PnL bridge</SectionTitle>
        <ReconciliationBadges account={document} current={current} />
        <Bridge
          components={document.components}
          rows={BRIDGE}
          caption="How the booked net PnL arises, term by term"
          definitions={diagnostics.definitions.attribution}
        />
        <Bridge
          components={document.components}
          rows={SIGNAL_BRIDGE}
          caption="The midpoint inventory PnL from the signal midpoints"
          definitions={diagnostics.definitions.attribution}
        />
        <Facts
          facts={(['market_value_rounding', 'notional_rounding', 'inventory_pnl_at_bid'] as const).map((component) => ({
            label: COMPONENT_LABEL[component],
            value: <FigureText figure={document.components[component]} signed={component.endsWith('pnl')} />,
          }))}
        />
      </div>
      <div className={styles.section}>
        <SectionTitle trust={trust}>Account, orders, and timing</SectionTitle>
        <AccountFacts account={document} run={run} />
      </div>
      {account === 'strategy' && (
        <div className={styles.section}>
          <SectionTitle trust={trust}>Strategy minus its benchmarks</SectionTitle>
          <Deltas run={run} />
        </div>
      )}
      <div className={styles.section}>
        <SectionTitle trust={trust}>The strategy’s decisions, described after the fact</SectionTitle>
        <p className={styles.note}>
          {run.decisions.evaluated} decisions sampled; {run.decisions.skipped_decision_interval} rows skipped by the
          decision interval and {run.decisions.skipped_prediction_unavailable} without a prediction. {sentence(diagnostics.definitions.labels)}
        </p>
        <CohortsTable cohorts={cohorts} caption="Decision cohorts of this run" />
        <p className={styles.note}>{sentence(diagnostics.definitions.future_returns)}</p>
        <FutureReturns cohorts={cohorts} caption="Future midpoint returns of this run’s scored cohort items" />
        <Bins cohorts={cohorts} />
      </div>
      <div className={styles.section}>
        <SectionTitle trust={trust}>Holding episodes</SectionTitle>
        <Episodes diagnosticId={located.diagnosticId} runId={run.run_id} account={account} />
      </div>
      <div className={styles.section}>
        <SectionTitle trust={trust}>Fills</SectionTitle>
        <Fills diagnosticId={located.diagnosticId} runId={run.run_id} account={account} />
        <p className={styles.note}>
          A midpoint is taken between the quote’s own bid and ask. When one side of a quote is far from the other, its
          midpoint is too, and the split between spread paid and midpoint inventory PnL moves with it; their sum, and the
          booked net PnL, do not.
        </p>
      </div>
    </Panel>
  );
}

// --- Overviews ------------------------------------------------------------------------------------------

const POOL_ROWS: readonly {
  readonly label: string;
  readonly pick: (pool: AcrossPool | TradingPool) => Components;
}[] = [
  { label: ACCOUNT_LABEL.strategy, pick: (pool) => pool.accounts.strategy },
  { label: ACCOUNT_LABEL.buy_and_hold, pick: (pool) => pool.accounts.buy_and_hold },
  { label: ACCOUNT_LABEL.cash, pick: (pool) => pool.accounts.cash },
  { label: 'Strategy − buy and hold', pick: (pool) => pool.deltas.buy_and_hold },
  { label: 'Strategy − cash', pick: (pool) => pool.deltas.cash },
];

function coverageOf(pool: AcrossPool | TradingPool): ReactNode {
  const status = POOL_STATUS[pool.status];
  if (!('folds' in pool)) {
    const label = { complete: 'Every study, every fold', partial: 'Partial', unavailable: 'No study' }[pool.status];
    return (
      <span>
        <Badge tone={status.tone}>{label}</Badge>
        <span className={styles.reasonCode}>
          {pool.studies.included.length} of {pool.studies.included.length + pool.studies.missing.length} studies
          {pool.studies.missing.length > 0
            ? `; no completed run in ${pool.studies.missing.map((identifier) => shortId(identifier)).join(', ')}`
            : ''}
        </span>
      </span>
    );
  }
  return (
    <span>
      <Badge tone={status.tone}>{status.label}</Badge>
      <span className={styles.reasonCode}>
        {pool.folds.included.length} of {pool.folds.planned} folds
        {pool.folds.missing.length > 0 ? `; missing fold ${pool.folds.missing.join(', ')}` : ''}
      </span>
    </span>
  );
}

function PoolTable({
  pools,
  trust,
  caption,
  scenarios,
}: {
  readonly pools: Partial<Record<ModelName, Record<string, AcrossPool | TradingPool>>>;
  readonly trust: PoolTrust['trading'] | undefined;
  readonly caption: string;
  readonly scenarios: readonly string[];
}) {
  return (
    <ScrollingTable label={`${caption}, scrollable`}>
      <table className={historyStyles.table}>
        <caption className="visually-hidden">{caption}</caption>
        <thead>
          <tr>
            <th scope="col">Model</th>
            <th scope="col">Scenario</th>
            <th scope="col">Account</th>
            <th scope="col">Coverage</th>
            {OVERVIEW_COMPONENTS.map((component) => (
              <th key={component} scope="col">
                {COMPONENT_LABEL[component]}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {MODELS.flatMap((model) =>
            scenarios.flatMap((scenario) => {
              const pool = pools[model]?.[scenario];
              if (pool === undefined) {
                return [];
              }
              return POOL_ROWS.map((row) => (
                <tr key={`${model}|${scenario}|${row.label}`}>
                  <td data-label="Model">{MODEL_LABEL[model]}</td>
                  <td data-label="Scenario">{scenarioLabel(scenario)}</td>
                  <td data-label="Account">
                    {row.label}
                  </td>
                  <td data-label="Coverage">
                    {coverageOf(pool)}
                    <TrustBadge trust={trust?.[model]?.[scenario]} noun="pool" />
                  </td>
                  {OVERVIEW_COMPONENTS.map((component) => (
                    <td key={component} data-label={COMPONENT_LABEL[component]} className={historyStyles.number}>
                      <FigureText figure={row.pick(pool)[component]} signed={component.endsWith('pnl')} />
                    </td>
                  ))}
                </tr>
              ));
            }),
          )}
        </tbody>
      </table>
    </ScrollingTable>
  );
}

function StudyPools({ study, trust }: { readonly study: StudySection; readonly trust: PoolTrust | undefined }) {
  const scenarios = study.scenarios.map((scenario) => scenario.name);
  const pooled: readonly CohortEntry[] = MODELS.flatMap((model) => {
    const predictions = study.pooled.predictions[model];
    return [
      {
        name: `${MODEL_LABEL[model]}: ${COHORT_LABEL.scorable_predictions}`,
        cohort: predictions.cohort,
        trust: trust?.predictions[model],
      },
      ...scenarios.flatMap((scenario) => {
        const pool = study.pooled.trading[model][scenario];
        return pool === undefined
          ? []
          : (['sampled_decisions', 'order_scheduling_decisions', 'filled_orders'] as const).map((name) => ({
              name: `${MODEL_LABEL[model]}, ${scenarioLabel(scenario)}: ${COHORT_LABEL[name]}`,
              cohort: pool.cohorts[name],
              trust: trust?.trading[model]?.[scenario],
            }));
      }),
    ];
  });
  return (
    <Panel title={`${study.instrument}: pooled over folds`} meta="Independent reset accounts, summed exactly">
      <PoolTable
        pools={study.pooled.trading}
        trust={trust?.trading}
        caption={`${study.instrument} attribution pooled over folds`}
        scenarios={scenarios}
      />
      <CohortsTable cohorts={pooled} caption={`${study.instrument} cohorts pooled over folds`} />
      <FutureReturns cohorts={pooled} caption={`${study.instrument} future midpoint returns pooled over folds`} />
    </Panel>
  );
}

function RunsTable({
  diagnostics,
  verification,
  selected,
  account,
}: {
  readonly diagnostics: Diagnostics;
  readonly verification: DiagnosticsVerification;
  readonly selected: string | null;
  readonly account: SimulatedAccount;
}) {
  return (
    <Panel title="Every run" meta="Strategy account">
      <ScrollingTable label="Every run, scrollable">
        <table className={historyStyles.table}>
          <caption className="visually-hidden">Each study, fold, model, and scenario's run, explained</caption>
          <thead>
            <tr>
              <th scope="col">Run</th>
              <th scope="col">State</th>
              {OVERVIEW_COMPONENTS.map((component) => (
                <th key={component} scope="col">
                  {COMPONENT_LABEL[component]}
                </th>
              ))}
              <th scope="col">Fills</th>
              <th scope="col">Explain</th>
            </tr>
          </thead>
          <tbody>
            {diagnostics.studies.flatMap((study) =>
              study.folds.flatMap((fold) =>
                fold.runs.map((run) => {
                  const name = `${study.instrument} fold ${fold.index} · ${MODEL_LABEL[run.model]} · ${scenarioLabel(run.scenario)}`;
                  if (run.state !== 'completed') {
                    return (
                      <tr key={`${study.study_id}|${fold.index}|${run.model}|${run.scenario}`}>
                        <td data-label="Run">
                          {name}
                        </td>
                        <td data-label="State">
                          <span>
                            <Badge tone="neutral">Not attributed</Badge>
                            <span className={styles.reasonCode}>{diagnosticReason(run.reason)}</span>
                          </span>
                        </td>
                        <td data-label="Figures" colSpan={OVERVIEW_COMPONENTS.length + 2}>
                          <Unavailable>No run to explain</Unavailable>
                        </td>
                      </tr>
                    );
                  }
                  const strategy = run.accounts.strategy;
                  const reconciled = strategy.reconciliation.booked_net_pnl;
                  const trust = runTrust(verification, run.run_id);
                  const words = trusted(trust) ? RECONCILIATION : RECORDED_RECONCILIATION;
                  return (
                    <tr key={run.run_id} aria-current={run.run_id === selected ? 'true' : undefined}>
                      <td data-label="Run">
                        {name}
                      </td>
                      <td data-label="State">
                        <span>
                          {'state' in reconciled ? (
                            <Badge tone={words[reconciled.state].tone}>{words[reconciled.state].label}</Badge>
                          ) : (
                            <Badge tone="caution">Not checkable</Badge>
                          )}
                          <TrustBadge trust={trust} noun="run" />
                        </span>
                      </td>
                      {OVERVIEW_COMPONENTS.map((component) => (
                        <td key={component} data-label={COMPONENT_LABEL[component]} className={historyStyles.number}>
                          <FigureText figure={strategy.components[component]} signed={component.endsWith('pnl')} />
                        </td>
                      ))}
                      <td data-label="Fills" className={historyStyles.number}>
                        {strategy.fills}
                      </td>
                      <td data-label="Explain">
                        <a
                          href={diagnosticsHref({
                            diagnosticId: diagnostics.diagnostic_id,
                            runId: run.run_id,
                            account,
                          })}
                        >
                          {run.run_id === selected ? 'Shown above' : `Explain ${shortId(run.run_id)}`}
                        </a>
                      </td>
                    </tr>
                  );
                }),
              ),
            )}
          </tbody>
        </table>
      </ScrollingTable>
    </Panel>
  );
}

function AcrossStudies({ diagnostics, trust }: { readonly diagnostics: Diagnostics; readonly trust: PoolTrust | undefined }) {
  const across = diagnostics.across_studies;
  if (across === undefined) {
    return null;
  }
  const scenarios = across.scenarios.map((scenario) => scenario.name);
  return (
    <Panel title="Across studies" meta="Independent accounts in different instruments, summed: not a portfolio">
      <PoolTable
        pools={across.trading}
        trust={trust?.trading}
        caption="Attribution pooled over every study"
        scenarios={scenarios}
      />
      {across.unshared_scenarios.length > 0 && (
        <p className={styles.note}>
          Not pooled, because not every study has them identically:{' '}
          {across.unshared_scenarios.map((entry) => `${scenarioLabel(entry.scenario)} (${shortId(entry.study_id)})`).join(', ')}.
        </p>
      )}
    </Panel>
  );
}

function Definitions({ diagnostics }: { readonly diagnostics: Diagnostics }) {
  const definitions = diagnostics.definitions;
  return (
    <Panel title="Definitions and how to check them">
      <ul className={styles.list}>
        {definitions.identities.map((text) => (
          <li key={text}>
            <Code>{text}</Code>
          </li>
        ))}
        {[
          definitions.symbols,
          definitions.scales,
          definitions.unavailable,
          definitions.joins,
          definitions.timing,
          definitions.episodes,
          definitions.labels,
          definitions.ratios,
          definitions.future_returns,
          definitions.pooling,
          definitions.not_computed,
        ].map((text) => (
          <li key={text}>{sentence(text)}</li>
        ))}
        {Object.entries(definitions.cohorts).map(([name, text]) => (
          <li key={name}>
            {COHORT_LABEL[name as keyof typeof COHORT_LABEL]}: {text}.
          </li>
        ))}
        <li>
          Confidence bins, fixed before any result: {diagnostics.confidence_bins.map((bin) => bin.name).join(', ')}; each
          includes its lower end, and the last its upper end too.
        </li>
      </ul>
      <p className={styles.note}>
        Rebuild these diagnostics from their sources and compare bytes:{' '}
        <Code>{`python -m chronos_research inspect-strategy-diagnostics --tools <build> --artifacts <artifacts> --diagnostics ${diagnostics.diagnostic_id}`}</Code>
      </p>
      <p className={styles.note}>
        Audit every figure independently with the standard library alone:{' '}
        <Code>{`python -I -S tools/strategy_diagnostics_audit/audit.py <artifacts> ${diagnostics.diagnostic_id}`}</Code>
      </p>
    </Panel>
  );
}

export function DiagnosticsDetailView({
  diagnosticId,
  runId,
  account,
}: {
  readonly diagnosticId: string;
  readonly runId: string | null;
  readonly account: SimulatedAccount;
}) {
  const detail = useDiagnostics(diagnosticId);
  if (detail.status === 'pending') {
    return (
      <Panel title="Strategy diagnostics">
        <p role="status">Reading and verifying the diagnostics…</p>
      </Panel>
    );
  }
  if (detail.status === 'error') {
    return (
      <Panel title="Strategy diagnostics">
        <ResearchFailure
          error={detail.error}
          title="The diagnostics could not be read"
          onRetry={() => void detail.refetch()}
          retrying={detail.isFetching}
        />
      </Panel>
    );
  }
  const { diagnostics, verification } = detail.data;
  const problems = sourceProblems(verification);
  const located = runId === null ? null : locate(diagnostics, runId);
  return (
    <>
      {problems.length > 0 && (
        <Alert tone="negative" title="Some sources no longer match what the diagnostics read">
          <p>
            Checked again now, these differ from what the diagnostics were built from. Every run, cohort, and pool that
            rests on one of them is shown as recorded and labelled unverified; the rest are unaffected. Generate the
            diagnostics again once the sources are restored.
          </p>
          <ul className={styles.list}>
            {problems.map((text) => (
              <li key={text}>{text}</li>
            ))}
          </ul>
        </Alert>
      )}
      {diagnostics.studies.some((study) => study.evidence === 'synthetic') && <SyntheticNotice />}
      {diagnostics.status === 'partial' && (
        <Alert tone="caution" title="These diagnostics are partial">
          <p>Some planned runs did not complete, so nothing explains them, and nothing missing is counted as zero:</p>
          <ul className={styles.list}>
            {diagnostics.studies.flatMap((study) =>
              study.missing.map((entry) => (
                <li key={`${study.study_id}-${entry.fold}-${entry.item}`}>
                  {study.instrument} fold {entry.fold}, {entry.item.replaceAll(':', ' ').replaceAll('_', ' ')}:{' '}
                  {diagnosticReason(entry.reason)} <span className={styles.reasonCode}>{entry.reason}</span>
                </li>
              )),
            )}
          </ul>
        </Alert>
      )}
      <Alert tone="info" title="An exact explanation, not evidence of skill">
        <p>
          Every figure is exact arithmetic on these runs’ own journals and recordings. Nothing is annualized, tested for
          significance, ranked, or declared a winner, and labels are read only to describe what happened afterwards.
        </p>
      </Alert>
      <Summary diagnostics={diagnostics} />
      <VerificationPanel diagnostics={diagnostics} verification={verification} />
      {runId !== null && located === null && (
        <Alert tone="caution" title="That run is not in these diagnostics">
          <p>Choose a run from the table below.</p>
        </Alert>
      )}
      {located !== null && (
        <RunDetail
          located={{ ...located, diagnosticId: diagnostics.diagnostic_id }}
          account={account}
          diagnostics={diagnostics}
          verification={verification}
        />
      )}
      <RunsTable diagnostics={diagnostics} verification={verification} selected={runId} account={account} />
      {diagnostics.studies.map((study) => (
        <StudyPools
          key={study.study_id}
          study={study}
          trust={verification.sources.now.figures.studies[study.study_id]?.pooled}
        />
      ))}
      <AcrossStudies diagnostics={diagnostics} trust={verification.sources.now.figures.across_studies} />
      <Definitions diagnostics={diagnostics} />
    </>
  );
}
