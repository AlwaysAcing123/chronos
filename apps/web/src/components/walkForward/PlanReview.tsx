import type { Plan } from '../../api/walkForwardSchemas';
import { nanosecondsAsSeconds } from '../../presentation/format';
import { sentence } from '../../presentation/models';
import { shortId } from '../../presentation/research';
import { EVIDENCE, scenarioLabel } from '../../presentation/walkForward';
import { notScoredReason } from '../../presentation/comparisons';
import { usePlan, useStartStudy } from '../../state/walkForward';
import historyStyles from '../history/history.module.css';
import { CONFIGURATION_FIELDS } from '../research/EvaluationForm';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ScrollingTable } from './ScrollingTable';
import { StudyJobStatus } from './StudyJobs';

function Ids({ children }: { readonly children: string }) {
  return <span className={styles.ids}>{children}</span>;
}

export function SyntheticNotice() {
  return (
    <Alert tone="caution" title="Synthetic recordings">
      <p>
        Every recording here was generated for tests. These folds exercise the fitting, scheduling, execution, and
        aggregation machinery; they say nothing about any market.
      </p>
    </Alert>
  );
}

function Folds({ plan }: { readonly plan: Plan }) {
  return (
    <Panel title="Folds and cutoffs" meta={`${String(plan.folds.length)} folds, expanding`}>
      <ScrollingTable label="Folds and cutoffs, scrollable">
        <table className={historyStyles.table}>
          <caption className="visually-hidden">Each fold's training recordings, cutoff, and evaluation recording</caption>
          <thead>
            <tr>
              <th scope="col">Fold</th>
              <th scope="col">Trains on</th>
              <th scope="col">Training rows</th>
              <th scope="col">Class support down · flat · up</th>
              <th scope="col">Latest training label known</th>
              <th scope="col">Information boundary</th>
              <th scope="col">Evaluates</th>
            </tr>
          </thead>
          <tbody>
            {plan.folds.map((fold) => {
              const support = fold.training.class_support;
              return (
                <tr key={fold.index}>
                  <td data-label="Fold">Fold {fold.index}</td>
                  <td data-label="Trains on" className={historyStyles.detail}>
                    <span className={styles.stackedFigures}>
                      {fold.training.members.map((member) => (
                        <span key={member.dataset_id}>
                          {member.stream}: <span className="numeric">{member.eligible_rows}</span> of{' '}
                          <span className="numeric">{member.feature_rows}</span> rows
                        </span>
                      ))}
                    </span>
                  </td>
                  <td data-label="Training rows" className={historyStyles.detail}>
                    <span>
                      <span>
                        <span className="numeric">{fold.training.eligible_rows}</span> eligible
                      </span>
                      {fold.training.members.flatMap((member) =>
                        Object.entries(member.excluded).map(([reason, count]) => (
                          <span key={`${member.dataset_id}-${reason}`} className={styles.reasonCode}>
                            {count} excluded in {member.stream}: {notScoredReason(reason)}
                          </span>
                        )),
                      )}
                    </span>
                  </td>
                  <td data-label="Class support down · flat · up" className={historyStyles.number}>
                    {support.down} · {support.flat} · {support.up}
                  </td>
                  <td data-label="Latest training label known">
                    {fold.training.max_label_available_at === null ? (
                      'No label'
                    ) : (
                      <MarketTime value={fold.training.max_label_available_at} stacked />
                    )}
                  </td>
                  <td data-label="Information boundary">
                    <span>
                      <MarketTime value={fold.information_boundary} stacked />
                      <span className={styles.reasonCode}>exclusive</span>
                    </span>
                  </td>
                  <td data-label="Evaluates" className={historyStyles.detail}>
                    <span>
                      <span>
                        {fold.evaluation.stream}: <span className="numeric">{fold.evaluation.feature_rows}</span> rows,
                        the whole recording
                      </span>
                      <span className={styles.reasonCode}>
                        <MarketTime value={fold.evaluation.window.start_at} /> to{' '}
                        <MarketTime value={fold.evaluation.window.end_at} /> (end exclusive)
                      </span>
                    </span>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </ScrollingTable>
      <p className={styles.note}>
        A training row needs available inputs, a usable label, and both its prediction time and its label’s availability
        time strictly before the fold’s information boundary. Every evaluation row is predicted, labelled or not.
      </p>
    </Panel>
  );
}

function Scenarios({ plan }: { readonly plan: Plan }) {
  return (
    <Panel title="Cost and latency scenarios" meta="Frozen before any result">
      <ScrollingTable label="Cost and latency scenarios, scrollable">
        <table className={historyStyles.table}>
          <caption className="visually-hidden">The frozen fee and latency scenarios</caption>
          <thead>
            <tr>
              <th scope="col">Scenario</th>
              <th scope="col">Fee per fill (USD)</th>
              <th scope="col">Latency (ns)</th>
            </tr>
          </thead>
          <tbody>
            {plan.scenarios.map((cell) => (
              <tr key={cell.name}>
                <td data-label="Scenario">
                  <span>
                    {scenarioLabel(cell.name)}
                    <span className={styles.reasonCode}>{cell.name}</span>
                  </span>
                </td>
                <td data-label="Fee per fill (USD)" className={historyStyles.number}>
                  {cell.fee_per_fill}
                </td>
                <td data-label="Latency (ns)" className={historyStyles.number}>
                  <span>
                    {cell.latency_ns}
                    <span className={styles.reasonCode}>{nanosecondsAsSeconds(cell.latency_ns)} s</span>
                  </span>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </ScrollingTable>
      <ul className={styles.list}>
        {plan.scenario_rules.map((rule) => (
          <li key={rule}>{sentence(rule)}</li>
        ))}
      </ul>
    </Panel>
  );
}

function Rules({ plan }: { readonly plan: Plan }) {
  return (
    <Panel title="Configuration and rules">
      <Facts facts={CONFIGURATION_FIELDS.map((field) => ({ label: field.label, value: plan.configuration[field.key] }))} />
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Definitions</h3>
        <ul className={styles.list}>
          {Object.entries(plan.definitions).map(([name, text]) => (
            <li key={name}>
              <strong>{name.replaceAll('_', ' ')}:</strong> {text}
            </li>
          ))}
          <li>
            <strong>scoring:</strong> {plan.scoring.rule}; {plan.scoring.unscored}
          </li>
        </ul>
      </div>
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Limits</h3>
        <Facts
          facts={Object.entries(plan.workload.caps).map(([name, value]) => ({
            label: `At most ${name.replaceAll('_', ' ')}`,
            value,
          }))}
        />
      </div>
    </Panel>
  );
}

function Start({ plan }: { readonly plan: Plan }) {
  const start = useStartStudy();
  return (
    <div className={styles.formPanel}>
      <Panel title="Start the study" meta={`${plan.workload.strategy_runs} engine runs`}>
        <p className={styles.note}>
          Starting sends only this plan’s ID. The research service verifies every recording again, refits each fold’s
          models, and runs {plan.workload.schedules} schedules through the engine {plan.scenarios.length} time
          {plan.scenarios.length === 1 ? '' : 's'} each, every run replayed. A changed source fails the study before
          anything runs.
        </p>
        {start.data !== undefined ? (
          <StudyJobStatus jobId={start.data.job.job_id} />
        ) : (
          start.error !== null && <ResearchFailure error={start.error} title="The study was not started" />
        )}
        <div className={styles.actions}>
          {start.data === undefined && (
            <Button
              variant="primary"
              disabled={start.isPending}
              onClick={() => {
                start.mutate(plan.plan_id);
              }}
            >
              {start.isPending ? 'Starting…' : 'Start study'}
            </Button>
          )}
        </div>
      </Panel>
    </div>
  );
}

// A published plan, read by its ID: everything it fixes before any result.
export function PlanReview({ planId }: { readonly planId: string }) {
  const plan = usePlan(planId);
  if (plan.status === 'pending') {
    return (
      <Panel title="Plan">
        <p role="status">Reading and verifying the plan…</p>
      </Panel>
    );
  }
  if (plan.status === 'error') {
    return (
      <Panel title="Plan">
        <ResearchFailure
          error={plan.error}
          title="The plan could not be read"
          onRetry={() => void plan.refetch()}
          retrying={plan.isFetching}
        />
      </Panel>
    );
  }
  const data = plan.data;
  const evidence = EVIDENCE[data.collection.evidence];
  return (
    <>
      {data.collection.evidence === 'synthetic' && <SyntheticNotice />}
      <div className={styles.grid}>
        <div className={styles.column}>
          <Panel title={`${data.instrument} · ${String(data.folds.length)} folds`} meta={shortId(data.plan_id)}>
            <Facts
              facts={[
                { label: 'Plan ID', value: <Ids>{data.plan_id}</Ids> },
                { label: 'Request SHA-256', value: <Ids>{data.request_sha256}</Ids> },
                {
                  label: 'Collection',
                  value: (
                    <>
                      <Ids>{data.collection.collection_id}</Ids> <Badge tone={evidence.tone}>{evidence.label}</Badge>
                    </>
                  ),
                },
                { label: 'Instrument', value: data.instrument },
                { label: 'Label horizon', value: `${nanosecondsAsSeconds(data.horizon_ns)} s` },
                { label: 'Earliest fold trains on', value: `${data.request.minimum_training_sources} recording(s)` },
                { label: 'Recordings without the instrument', value: String(data.collection.skipped_members.length) },
                { label: 'Training rows, all folds', value: data.workload.training_rows },
                { label: 'Evaluation rows, all folds', value: data.workload.evaluation_rows },
                { label: 'Engine runs', value: data.workload.strategy_runs },
              ]}
            />
          </Panel>
        </div>
        <div className={styles.column}>
          <Start plan={data} />
        </div>
      </div>
      <Folds plan={data} />
      <Scenarios plan={data} />
      <Rules plan={data} />
    </>
  );
}
