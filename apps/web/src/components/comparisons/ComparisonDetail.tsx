import type { ReactNode } from 'react';

import type {
  AccountMetrics,
  ArtifactCheck,
  Comparison,
  ComparisonMember,
  DeltaPair,
  Predictive,
} from '../../api/comparisonSchemas';
import { MEMBER_STATE, artifactCheck, memberReason, notScoredReason } from '../../presentation/comparisons';
import { CUTOFF_BASIS, ORIGIN_LABEL, familyLabel, modelName, sentence } from '../../presentation/models';
import { WINDOW_LABEL, shortId } from '../../presentation/research';
import { useComparison } from '../../state/comparisons';
import { modelsHref, researchHref } from '../../state/route';
import historyStyles from '../history/history.module.css';
import { MetricValue } from '../research/MetricValue';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { DecimalText, Unavailable } from '../ui/DecimalText';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';

function Ids({ children }: { readonly children: string }) {
  return <span className={styles.ids}>{children}</span>;
}

// One figure of one member: its value, or why there is none.
type Figure = (member: ComparisonMember) => ReactNode;

function whyMissing(member: ComparisonMember): ReactNode {
  return <Unavailable>{member.state === 'completed' ? 'Unavailable' : MEMBER_STATE[member.state].label}</Unavailable>;
}

function strategy(figure: (account: AccountMetrics) => ReactNode): Figure {
  return (member) => (member.accounts === undefined ? whyMissing(member) : figure(member.accounts.strategy));
}

function valuation(key: 'net_total_pnl' | 'return_percent' | 'fees_paid' | 'equity', signed = false): Figure {
  return strategy((account) =>
    'unavailable' in account.valuation ? (
      <Unavailable>{`Unavailable: ${account.valuation.unavailable.replaceAll('_', ' ')}`}</Unavailable>
    ) : (
      <DecimalText value={account.valuation[key]} signed={signed} suffix={key === 'return_percent' ? '%' : ''} />
    ),
  );
}

function delta(against: 'buy_and_hold' | 'cash' | 'reference', key: keyof DeltaPair): Figure {
  return (member) => {
    if (member.deltas === undefined) {
      return whyMissing(member);
    }
    const pair = member.deltas[against];
    if (pair === undefined) {
      return <Unavailable>The reference itself</Unavailable>;
    }
    const value = pair[key];
    return 'unavailable' in value ? (
      <Unavailable>{`Unavailable: ${value.unavailable.replaceAll('_', ' ')}`}</Unavailable>
    ) : (
      <DecimalText value={value.value} signed suffix={key === 'return_percent' ? ' pp' : ''} />
    );
  };
}

const ROWS: readonly { readonly label: string; readonly figure: Figure }[] = [
  { label: 'Net PnL (USD)', figure: valuation('net_total_pnl', true) },
  { label: 'Return', figure: valuation('return_percent', true) },
  { label: 'Net PnL minus buy and hold', figure: delta('buy_and_hold', 'net_total_pnl') },
  { label: 'Net PnL minus cash', figure: delta('cash', 'net_total_pnl') },
  { label: 'Net PnL minus the reference', figure: delta('reference', 'net_total_pnl') },
  { label: 'Return minus the reference', figure: delta('reference', 'return_percent') },
  { label: 'Fees paid (USD)', figure: valuation('fees_paid') },
  { label: 'Largest drawdown (USD)', figure: strategy((account) => <DecimalText value={account.drawdown.maximum} />) },
  { label: 'Fills', figure: strategy((account) => <span className="numeric">{account.fills}</span>) },
  {
    label: 'Orders filled · rejected · expired',
    figure: strategy((account) => (
      <span className="numeric">
        {account.orders.filled} · {account.orders.rejected} · {account.orders.expired}
      </span>
    )),
  },
  { label: 'Turnover (USD)', figure: strategy((account) => <DecimalText value={account.turnover.notional} />) },
  {
    label: 'Rows predicted',
    figure: (member) =>
      member.schedule === undefined ? (
        whyMissing(member)
      ) : (
        <span className="numeric">
          {member.schedule.counts.predicted} of {member.schedule.counts.rows}
        </span>
      ),
  },
  {
    label: 'Predicted down · flat · up',
    figure: (member) =>
      member.schedule === undefined ? (
        whyMissing(member)
      ) : (
        <span className="numeric">
          {['down', 'flat', 'up'].map((name) => member.schedule?.counts.classes[name] ?? '0').join(' · ')}
        </span>
      ),
  },
];

type Scored = Exclude<Predictive, { readonly unavailable: string }>;

// A figure of the member's prediction quality, or why it was not scored.
function scored(figure: (scores: Scored) => ReactNode): Figure {
  return (member) => {
    const scores = member.predictive;
    if (scores === undefined) {
      return whyMissing(member);
    }
    return 'unavailable' in scores ? (
      <Unavailable>{`Not scored: ${scores.unavailable.replaceAll('_', ' ')}`}</Unavailable>
    ) : (
      figure(scores)
    );
  };
}

const CLASSES = ['down', 'flat', 'up'] as const;

const PREDICTIVE_ROWS: readonly { readonly label: string; readonly figure: Figure }[] = [
  { label: 'Accuracy', figure: scored((scores) => <MetricValue metric={scores.metrics.accuracy} />) },
  { label: 'Balanced accuracy', figure: scored((scores) => <MetricValue metric={scores.metrics.balanced_accuracy} />) },
  { label: 'Log loss', figure: scored((scores) => <MetricValue metric={scores.metrics.log_loss} as="number" />) },
  { label: 'Brier score', figure: scored((scores) => <MetricValue metric={scores.metrics.brier_score} as="number" />) },
  {
    label: 'Recall down · flat · up',
    figure: scored((scores) => (
      <span className={styles.stackedFigures}>
        {CLASSES.map((name) => (
          <MetricValue key={name} metric={scores.metrics.per_class[name].recall} />
        ))}
      </span>
    )),
  },
  {
    label: 'Rows scored',
    figure: scored((scores) => (
      <span className="numeric">
        {scores.rows.scored} of {scores.rows.scheduled}
      </span>
    )),
  },
  {
    label: 'Rows not scored',
    figure: scored((scores) => {
      const reasons = Object.entries(scores.rows.not_scored);
      return reasons.length === 0 ? (
        'None'
      ) : (
        <span className={styles.stackedFigures}>
          {reasons.map(([reason, count]) => (
            <span key={reason}>
              <span className="numeric">{count}</span> {notScoredReason(reason)}
            </span>
          ))}
        </span>
      );
    }),
  },
];

function MemberHeading({ member, reference }: { readonly member: ComparisonMember; readonly reference: boolean }) {
  const state = MEMBER_STATE[member.state];
  const cutoff = member.information_cutoff;
  return (
    <th scope="col">
      <a href={modelsHref(member.model_version_id)}>{modelName(member)}</a>
      <span className={styles.reasonCode}>
        {member.family === undefined ? shortId(member.model_version_id) : familyLabel(member.family)}
        {reference ? ' · reference' : ''}
      </span>
      <div>
        <Badge tone={state.tone}>{state.label}</Badge>
        {cutoff !== undefined && <Badge tone={CUTOFF_BASIS[cutoff.basis].tone}>{CUTOFF_BASIS[cutoff.basis].label}</Badge>}
      </div>
    </th>
  );
}

function MemberTable({
  comparison,
  rows,
  caption,
}: {
  readonly comparison: Comparison;
  readonly rows: readonly { readonly label: string; readonly figure: Figure }[];
  readonly caption: string;
}) {
  return (
    <div className={historyStyles.scroll}>
      <table className={historyStyles.table}>
        <caption className="visually-hidden">{caption}</caption>
          <thead>
            <tr>
              <th scope="col">Figure</th>
              {comparison.members.map((member) => (
                <MemberHeading
                  key={member.model_version_id}
                  member={member}
                  reference={member.model_version_id === comparison.reference_model_version_id}
                />
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((row) => (
              <tr key={row.label}>
                <th scope="row">{row.label}</th>
                {comparison.members.map((member) => (
                  // On a phone each cell is labelled with its member.
                  <td key={member.model_version_id} className={historyStyles.number} data-label={modelName(member)}>
                    {row.figure(member)}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
      </table>
    </div>
  );
}

function SideBySide({ comparison }: { readonly comparison: Comparison }) {
  return (
    <Panel title="Side by side" meta="Strategy account of each member">
      <MemberTable
        comparison={comparison}
        rows={ROWS}
        caption="Each member's strategy results, one column per member"
      />
      <p className={styles.note}>
        Differences are computed exactly by the research service. Return differences are in percentage points. A member
        that did not complete shows why, never a zero.
      </p>
    </Panel>
  );
}

function PredictionQuality({ comparison }: { readonly comparison: Comparison }) {
  return (
    <Panel title="Prediction quality" meta="Against the window's realized labels">
      <MemberTable
        comparison={comparison}
        rows={PREDICTIVE_ROWS}
        caption="Each member's classification quality against realized labels, one column per member"
      />
      <p className={styles.note}>
        Scored by the research service after every member was predicted, from each member's stored predictions, with
        the experiment's definitions. Classification quality is not trading performance. A metric without a value says
        why.
      </p>
    </Panel>
  );
}

function Benchmarks({ comparison }: { readonly comparison: Comparison }) {
  const { benchmarks } = comparison;
  if ('unavailable' in benchmarks) {
    return (
      <Panel title="Benchmarks">
        <p className={styles.note}>
          <Unavailable>No member completed, so no benchmark ran.</Unavailable>
        </p>
      </Panel>
    );
  }
  const account = (name: 'buy_and_hold' | 'cash') => {
    const value = benchmarks.accounts[name].valuation;
    return 'unavailable' in value ? (
      <Unavailable>{`Unavailable: ${value.unavailable}`}</Unavailable>
    ) : (
      <DecimalText value={value.net_total_pnl} signed />
    );
  };
  return (
    <Panel
      title="Benchmarks"
      meta={
        <Badge tone={benchmarks.identical_across_members ? 'positive' : 'negative'}>
          {benchmarks.identical_across_members ? 'Identical for every member' : 'Differ between members'}
        </Badge>
      }
    >
      <Facts
        facts={[
          { label: 'Buy and hold net PnL (USD)', value: account('buy_and_hold') },
          { label: 'Cash net PnL (USD)', value: account('cash') },
        ]}
      />
      {!benchmarks.identical_across_members && (
        <Alert tone="negative" title="The benchmarks differ between members">
          <p>{benchmarks.members_differing.map(shortId).join(', ')}</p>
        </Alert>
      )}
    </Panel>
  );
}

function Reproducibility({ comparison, checks }: { readonly comparison: Comparison; readonly checks: readonly ArtifactCheck[] }) {
  return (
    <Panel title="Members and reproducibility">
      <ul className={styles.list}>
        {comparison.members.map((member) => {
          const check = checks.find((entry) => entry.model_version_id === member.model_version_id);
          return (
            <li key={member.model_version_id}>
              <strong>{modelName(member)}.</strong>{' '}
              {member.origin !== undefined && `${ORIGIN_LABEL[member.origin]}. `}
              {member.state === 'completed' ? (
                <>
                  Schedule rebuilt identically: {member.schedule?.reproduced === true ? 'yes' : 'no'}; run replayed
                  identically: {member.run?.replayed === true ? 'yes' : 'no'}.{' '}
                </>
              ) : (
                <>
                  {memberReason(member.reason ?? '')}.{' '}
                  {member.detail !== undefined && sentence(member.detail)}
                  <span className={styles.reasonCode}>{member.reason}</span>{' '}
                </>
              )}
              {member.conditional === true && 'Its result is conditional on its uploader’s declared cutoff. '}
              {member.schedule !== undefined && (
                <>
                  Schedule <Ids>{shortId(member.schedule.artifact_id)}</Ids>{' '}
                  <Badge tone={artifactCheck(check?.schedule).tone}>{artifactCheck(check?.schedule).label}</Badge>{' '}
                </>
              )}
              {member.run !== undefined && (
                <>
                  Run <a href={researchHref(member.run.artifact_id)}>{shortId(member.run.artifact_id)}</a>{' '}
                  <Badge tone={artifactCheck(check?.run).tone}>{artifactCheck(check?.run).label}</Badge>
                </>
              )}
            </li>
          );
        })}
      </ul>
    </Panel>
  );
}

function Summary({ comparison }: { readonly comparison: Comparison }) {
  const { window, status } = comparison;
  return (
    <Panel title={`${comparison.spec.instrument} · ${WINDOW_LABEL[window.name]} window`} meta={shortId(comparison.comparison_id)}>
      <Facts
        facts={[
          { label: 'Comparison ID', value: <Ids>{comparison.comparison_id}</Ids> },
          { label: 'Spec SHA-256', value: <Ids>{comparison.spec_sha256}</Ids> },
          { label: 'Split', value: <Ids>{comparison.sources.split_artifact_id}</Ids> },
          { label: 'Stream', value: comparison.sources.stream },
          { label: 'Window starts', value: <MarketTime value={window.start_at} stacked /> },
          {
            label: 'Window ends',
            value: window.end_at === null ? 'End of the data' : <MarketTime value={window.end_at} stacked />,
          },
          {
            label: 'Members',
            value: `${status.completed} completed, ${status.refused} refused, ${status.failed} failed`,
          },
        ]}
      />
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Why the comparison is fair</h3>
        <ul className={styles.list}>
          {comparison.fairness.map((text) => (
            <li key={text}>{sentence(text)}</li>
          ))}
        </ul>
      </div>
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>How prediction quality is scored</h3>
        <ul className={styles.list}>
          {comparison.scoring.map((text) => (
            <li key={text}>{sentence(text)}</li>
          ))}
        </ul>
      </div>
    </Panel>
  );
}

export function ComparisonDetailView({ comparisonId }: { readonly comparisonId: string }) {
  const detail = useComparison(comparisonId);
  if (detail.status === 'pending') {
    return (
      <Panel title="Comparison">
        <p role="status">Reading and verifying the comparison…</p>
      </Panel>
    );
  }
  if (detail.status === 'error') {
    return (
      <Panel title="Comparison">
        <ResearchFailure
          error={detail.error}
          title="The comparison could not be read"
          onRetry={() => void detail.refetch()}
          retrying={detail.isFetching}
        />
      </Panel>
    );
  }
  const { comparison, artifacts } = detail.data;
  const conditional = comparison.members.filter((member) => member.conditional === true);
  return (
    <>
      {conditional.length > 0 && (
        <Alert tone="caution" title="Some results rest on declared cutoffs">
          <p>
            {conditional.map(modelName).join(', ')}: Chronos did not verify when these models stopped seeing data, so
            their results are out of sample only if their uploaders' declarations are true.
          </p>
        </Alert>
      )}
      <Summary comparison={comparison} />
      <SideBySide comparison={comparison} />
      <PredictionQuality comparison={comparison} />
      <div className={styles.grid}>
        <div className={styles.column}>
          <Benchmarks comparison={comparison} />
        </div>
        <div className={styles.column}>
          <Reproducibility comparison={comparison} checks={artifacts} />
        </div>
      </div>
    </>
  );
}
