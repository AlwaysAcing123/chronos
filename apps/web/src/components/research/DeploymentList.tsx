import { Fragment, useId, useState } from 'react';
import type { ReactNode } from 'react';

import type { Condition, Decision, Deployment, DeploymentState, RunCounters } from '../../api/deployments';
import { isUncertainDeploymentControl } from '../../api/deployments';
import { ChronosApiError } from '../../api/errors';
import type { Timestamp } from '../../api/schemas';
import {
  decisionIntent,
  decisionOutcome,
  durationText,
  explainCondition,
  microsecondsAsMilliseconds,
  outcomeCode,
  policyReasonText,
} from '../../presentation/deployments';
import { CUTOFF_BASIS, familyLabel } from '../../presentation/models';
import { MODEL_LABEL, shortId } from '../../presentation/research';
import type { Tone } from '../../presentation/vocabulary';
import {
  useDecisions,
  useDeploymentClient,
  useDeployments,
  useKillSwitch,
  useStartDeployment,
  useStopDeployment,
} from '../../state/deployments';
import { modelsHref } from '../../state/route';
import historyStyles from '../history/history.module.css';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ListFooter } from './ListFooter';
import styles from './research.module.css';

const STATE: Readonly<Record<DeploymentState, { readonly label: string; readonly tone: Tone }>> = {
  running: { label: 'Running', tone: 'positive' },
  stopping: { label: 'Stopping', tone: 'caution' },
  stopped: { label: 'Stopped', tone: 'neutral' },
  faulted: { label: 'Faulted', tone: 'negative' },
};

// Why a deployment is in its state, for reading.
const REASON: Readonly<Record<string, string>> = {
  created: 'Created; not started yet',
  started: 'Started',
  stop_requested: 'Stopped on request',
  kill_switch: 'Stopped by the kill switch',
  server_restarted: 'Stopped because the server restarted; it never resumes on its own',
  server_stopped: 'Stopped when the server shut down',
  model_identity_mismatch: 'The model no longer matches the one it was created with',
  order_limit_reached: 'The run reached its maximum number of orders',
  rejection_limit_reached: 'Too many consecutive orders were rejected',
  inference_timeout: 'The model did not answer in time',
  inference_worker_exited: 'The model worker ended',
  inference_protocol_error: 'The model worker answered incorrectly',
  account_unhealthy: 'The deployment’s account is halted',
  runtime_error: 'An internal failure stopped it',
};

// The deployment's model: an experiment's, or a registered version, linked.
function DeploymentModel({ model }: { readonly model: Deployment['model'] }) {
  return model.reference === 'experiment' ? (
    <>{`${model.instrument} · ${MODEL_LABEL[model.name]} · ${shortId(model.experiment_id)}`}</>
  ) : (
    <>
      {`${model.instrument} · ${familyLabel(model.name)} · `}
      <a href={modelsHref(model.model_version_id)}>{`version ${shortId(model.model_version_id)}`}</a>
    </>
  );
}

function failureText(error: unknown): string {
  if (error instanceof ChronosApiError) {
    const failure = error.failure;
    if (failure.kind === 'api_error') {
      return `${failure.error.message} (${failure.error.reason ?? failure.error.code})`;
    }
    return failure.detail;
  }
  return error instanceof Error ? error.message : 'Unknown failure';
}

// A machine code that may wrap after its underscores, so a narrow column
// wraps it rather than clipping it. Its text is unchanged.
function WrappingCode({ code }: { readonly code: string }) {
  return code.split('_').map((part, index) => (
    <Fragment key={index}>
      {index > 0 && (
        <>
          _<wbr />
        </>
      )}
      {part}
    </Fragment>
  ));
}

// Which run a saved decision belongs to, when it is not the current one.
function runNote(decision: Decision, deployment: Deployment): string {
  return decision.run_generation === deployment.run_generation ? '' : ` (run ${decision.run_generation}, an earlier run)`;
}

function DecisionRows({
  deployment,
  decisions,
}: {
  readonly deployment: Deployment;
  readonly decisions: ReturnType<typeof useDecisions>;
}) {
  if (decisions.status === 'pending') {
    return <p role="status">Loading decisions…</p>;
  }
  if (decisions.data === undefined) {
    return <p className={historyStyles.empty}>Decisions could not be read: {failureText(decisions.error)}</p>;
  }
  const rows: readonly Decision[] = decisions.data.decisions.slice(0, 10);
  if (rows.length === 0) {
    return <p className={historyStyles.empty}>No decisions saved yet.</p>;
  }
  return (
    <>
      {decisions.isRefetchError && (
        <p className={styles.note} role="status">
          These decisions may be out of date: the latest refresh failed.
        </p>
      )}
      {/* Focusable, as the history tables are, so a keyboard can scroll it. */}
      <div
        className={historyStyles.scroll}
        role="region"
        aria-label={`Latest saved decisions of ${deployment.deployment_id}`}
        tabIndex={0}
      >
        <table className={historyStyles.table}>
          <caption className="visually-hidden">Latest saved decisions of {deployment.deployment_id}, newest first</caption>
          <thead>
            <tr>
              <th scope="col">Observation (UTC, market)</th>
              <th scope="col">Prediction</th>
              <th scope="col">Intent</th>
              <th scope="col">Outcome</th>
            </tr>
          </thead>
          <tbody>
            {rows.map((decision) => {
              const outcome = decisionOutcome(decision);
              const code = outcomeCode(decision);
              return (
                <tr key={decision.decision_sequence}>
                  <td data-label="Observation (UTC, market)">
                    <MarketTime value={decision.observation.received_at} stacked />
                    {runNote(decision, deployment) !== '' && (
                      <span className={styles.reasonCode}>{`run ${decision.run_generation}`}</span>
                    )}
                  </td>
                  <td data-label="Prediction">
                    <div>
                      {decision.prediction.class}
                      <span className={styles.reasonCode}>{`confidence ${decision.prediction.confidence}`}</span>
                    </div>
                  </td>
                  <td data-label="Intent">
                    <div>
                      {decisionIntent(decision)}
                      <span className={styles.reasonCode}>
                        <WrappingCode code={decision.policy_reason} />
                      </span>
                    </div>
                  </td>
                  <td className={styles.decisionOutcome} data-label="Outcome">
                    <div>
                      <Badge tone={outcome.tone}>{outcome.label}</Badge>
                      {outcome.detail !== null && ` ${outcome.detail}`}
                      {code !== null && (
                        <span className={styles.reasonCode}>
                          <WrappingCode code={code} />
                        </span>
                      )}
                      {decision.fill != null && decision.timing != null && (
                        <span className={styles.reasonCode}>
                          {`+${durationText(decision.timing.signal_to_fill)} after the signal (market)`}
                        </span>
                      )}
                    </div>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </>
  );
}

// The times a condition names, each with its clock.
function ConditionTimes({ condition }: { readonly condition: Condition }) {
  const times: { readonly label: string; readonly value: Timestamp }[] = [];
  const add = (label: string, value: Timestamp | null) => {
    if (value !== null) times.push({ label, value });
  };
  add('training cutoff', condition.training_cutoff);
  add('latest quote received', condition.observation_received_at);
  add('next eligible quote from', condition.next_eligible_received_at);
  add('not before', condition.not_before_received_at);
  add('expires', condition.expires_at);
  add('as of (market clock)', condition.observed_at_market);
  if (times.length === 0 && condition.observed_at_wall === null) {
    return null;
  }
  return (
    <span className={styles.choiceDetail}>
      {times.map((time) => (
        <span key={time.label}>
          {` ${time.label} `}
          <MarketTime value={time.value} />
        </span>
      ))}
      {condition.observed_at_wall !== null && (
        <span>
          {' · server time '}
          <MarketTime value={condition.observed_at_wall} />
        </span>
      )}
    </span>
  );
}

function NowLine({ deployment }: { readonly deployment: Deployment }) {
  const condition = deployment.condition;
  if (condition === null) {
    return <span>Unknown: this server does not report what the deployment is doing.</span>;
  }
  const explained = explainCondition(condition, deployment.model.instrument);
  return (
    <span>
      <Badge tone={explained.tone}>{explained.label}</Badge> {explained.sentence}{' '}
      <span className={styles.reasonCode}>{condition.reason === null ? condition.code : `${condition.code}: ${condition.reason}`}</span>
      <ConditionTimes condition={condition} />
    </span>
  );
}

// Which observation a fact describes: the saved decisions and the status
// refresh separately, so one can be a refresh ahead of the other.
function ObservedAt({ decision }: { readonly decision: Decision }) {
  return (
    <span className={styles.reasonCode}>
      observation received <MarketTime value={decision.observation.received_at} /> (market clock)
    </span>
  );
}

function latestPrediction(decisions: ReturnType<typeof useDecisions>, deployment: Deployment): ReactNode {
  if (decisions.data === undefined) {
    return decisions.status === 'pending' ? 'Loading…' : 'Unavailable: the saved decisions could not be read';
  }
  const latest = decisions.data.decisions[0];
  if (latest === undefined) {
    return 'None saved yet';
  }
  return (
    <>
      {`${latest.prediction.class}, model confidence ${latest.prediction.confidence} (not a probability of profit) · ${decisionIntent(latest)}: ${policyReasonText(latest.policy_reason)} · ${decisionOutcome(latest).label}${runNote(latest, deployment)}`}
      <ObservedAt decision={latest} />
    </>
  );
}

function latestTiming(decisions: ReturnType<typeof useDecisions>): ReactNode {
  const latest = decisions.data?.decisions.find((decision) => decision.order !== null);
  if (latest?.timing == null) {
    return 'No order decision saved yet';
  }
  const timing = latest.timing;
  return (
    <>
      {`signal to decision ${durationText(timing.signal_to_decision)} · to decision resolved ${durationText(timing.signal_to_resolution)} · to fill ${durationText(timing.signal_to_fill)} · execution quote received ${durationText(timing.execution_quote_offset)} after the signal`}
      <ObservedAt decision={latest} />
    </>
  );
}

function RunCounts({ run, generation }: { readonly run: RunCounters; readonly generation: string }) {
  const entries = (counts: Record<string, string>) => Object.entries(counts).sort(([left], [right]) => left.localeCompare(right));
  const skipped = entries(run.skipped);
  const waits = entries(run.waits);
  return (
    <details className={styles.details}>
      <summary>{`Counts by reason (run ${generation}, this server process)`}</summary>
      <p className={styles.note}>
        Cumulative since this run started. They say how often each reason occurred, not what is happening now.
      </p>
      <ul className={styles.list}>
        <li>{`${run.candidates} quotes taken, ${run.superseded} replaced before they were taken`}</li>
        {skipped.map(([reason, count]) => (
          <li key={`skipped-${reason}`}>
            {`Skipped ${count}: `}
            <Code>{reason}</Code>
          </li>
        ))}
        {waits.map(([reason, count]) => (
          <li key={`wait-${reason}`}>
            {`Order waited ${count}: `}
            <Code>{reason}</Code>
          </li>
        ))}
      </ul>
    </details>
  );
}

function DeploymentCard({
  deployment,
  onOpenAccount,
}: {
  readonly deployment: Deployment;
  readonly onOpenAccount: ((accountId: string) => void) | undefined;
}) {
  const confirmId = useId();
  const [confirmed, setConfirmed] = useState(false);
  const start = useStartDeployment();
  const stop = useStopDeployment();
  const state = STATE[deployment.state];
  const target = { id: deployment.deployment_id, accountId: deployment.account_id };
  const startUnknown = isUncertainDeploymentControl(start.error);
  const stopUnknown = isUncertainDeploymentControl(stop.error);
  const canStart = (deployment.state === 'stopped' || deployment.state === 'faulted') && !startUnknown && !stopUnknown;
  const run = deployment.run;
  const decisions = useDecisions(deployment.deployment_id, deployment.active);
  return (
    <article className={styles.section} aria-label={`Deployment ${deployment.deployment_id}`}>
      <div className={styles.inline}>
        <h3 className={styles.sectionTitle}>{deployment.deployment_id}</h3>
        <Badge tone={state.tone}>{state.label}</Badge>
        <span className={styles.choiceDetail}>{REASON[deployment.state_reason] ?? deployment.state_reason}</span>
      </div>
      <Facts
        facts={[
          { label: 'Now', value: <NowLine deployment={deployment} /> },
          { label: 'Latest saved prediction', value: latestPrediction(decisions, deployment) },
          {
            label: 'Paper account',
            value:
              onOpenAccount === undefined ? (
                deployment.account_id
              ) : (
                <Button
                  variant="quiet"
                  onClick={() => {
                    onOpenAccount(deployment.account_id);
                  }}
                >
                  {`Open ${deployment.account_id} in trading`}
                </Button>
              ),
          },
          { label: 'Model', value: <DeploymentModel model={deployment.model} /> },
          { label: 'Parameters', value: <Code>{shortId(deployment.model.parameters_sha256)}</Code> },
          {
            label: 'Training cutoff (UTC)',
            value:
              deployment.model.training_cutoff_basis === 'declared' ? (
                <div>
                  <MarketTime value={deployment.model.training_cutoff} stacked />
                  <Badge tone={CUTOFF_BASIS.declared.tone}>{CUTOFF_BASIS.declared.label}</Badge>
                </div>
              ) : (
                <MarketTime value={deployment.model.training_cutoff} />
              ),
          },
          {
            label: 'Policy',
            value: `Threshold ${deployment.policy.threshold} · target ${deployment.policy.target_quantity}`,
          },
          {
            label: 'Configured timing (market clock)',
            value: `at least ${deployment.timing.latency_ms} ms from signal to order (a minimum, not a promise) · ${deployment.timing.decision_interval_ms} ms between decisions · signals expire after ${deployment.timing.max_signal_age_ms} ms`,
          },
          { label: 'Latest order timing (market clock)', value: latestTiming(decisions) },
          {
            label: 'This run',
            value:
              run === null
                ? `Run ${deployment.run_generation}: no counts in this server process`
                : `Run ${deployment.run_generation}: ${run.predictions} predictions, ${run.order_decisions} order decisions, ${run.orders_recorded} orders, ${run.abandoned} abandoned, ${run.superseded} superseded`,
          },
          ...(run === null
            ? []
            : [
                {
                  label: 'Work time (wall clock, this server process)',
                  value: `inference up to ${microsecondsAsMilliseconds(run.max_inference_us)} ms · trusted order path up to ${microsecondsAsMilliseconds(run.max_submission_us)} ms, including waits for the market`,
                },
              ]),
          {
            label: 'Latest decided observation',
            value:
              deployment.watermark === null ? 'None yet' : <MarketTime value={deployment.watermark.received_at} />,
          },
        ]}
      />
      {run !== null && <RunCounts run={run} generation={deployment.run_generation} />}
      {run?.last_error != null && (
        <Alert tone="caution" title="Latest problem">
          <p>{run.last_error}</p>
        </Alert>
      )}
      <div className={styles.actions}>
        {canStart ? (
          <>
            <label className={styles.choice} htmlFor={confirmId}>
              <input
                id={confirmId}
                type="checkbox"
                checked={confirmed}
                onChange={(event) => {
                  setConfirmed(event.target.checked);
                }}
              />
              {`Let this model place simulated orders in ${deployment.account_id}`}
            </label>
            <Button
              variant="primary"
              disabled={!confirmed || start.isPending}
              onClick={() => {
                start.mutate(target, {
                  onSettled: () => {
                    setConfirmed(false);
                  },
                });
              }}
            >
              {start.isPending ? 'Loading the model…' : 'Start'}
            </Button>
          </>
        ) : (
          <Button
            disabled={stop.isPending || deployment.state === 'stopping'}
            onClick={() => {
              stop.mutate(target, { onSuccess: () => { start.reset(); } });
            }}
          >
            {stop.isPending ? 'Stopping…' : 'Stop'}
          </Button>
        )}
      </div>
      {(start.error !== null || stop.error !== null) && (
        <Alert
          tone={startUnknown || stopUnknown ? 'caution' : 'negative'}
          title={stop.error !== null ? (stopUnknown ? 'Stop outcome unknown' : 'Stop request rejected') :
            (startUnknown ? 'Start outcome unknown' : 'Start request rejected')}
          live
        >
          {(startUnknown || stopUnknown) && (
            <p>
              The model may still be trading, even if the last displayed status is stopped. Status is being
              refreshed. Use Stop or the kill switch before requesting another Start.
            </p>
          )}
          <p>{failureText(stop.error ?? start.error)}</p>
        </Alert>
      )}
      <DecisionRows deployment={deployment} decisions={decisions} />
    </article>
  );
}

function KillSwitch({ active }: { readonly active: boolean }) {
  const [armed, setArmed] = useState(false);
  const kill = useKillSwitch();
  return (
    <Alert
      tone={active ? 'negative' : 'neutral'}
      title="Kill switch"
      live={kill.isSuccess}
      actions={
        armed ? (
          <>
            <Button
              variant="primary"
              disabled={kill.isPending}
              onClick={() => {
                kill.mutate(undefined, {
                  onSettled: () => {
                    setArmed(false);
                  },
                });
              }}
            >
              {kill.isPending ? 'Stopping everything…' : 'Stop every deployment now'}
            </Button>
            <Button
              disabled={kill.isPending}
              onClick={() => {
                setArmed(false);
              }}
            >
              Cancel
            </Button>
          </>
        ) : (
          <Button
            onClick={() => {
              setArmed(true);
            }}
          >
            Stop all deployments…
          </Button>
        )
      }
    >
      <p>
        Stops every deployment at once and keeps their positions. Each one trades again only when started again.
        {kill.isSuccess && ` Stopped ${String(kill.data.length)}.`}
      </p>
      {kill.error !== null && <p>{failureText(kill.error)}</p>}
    </Alert>
  );
}

export function DeploymentList({ onOpenAccount }: { readonly onOpenAccount?: ((accountId: string) => void) | undefined }) {
  const client = useDeploymentClient();
  const deployments = useDeployments();
  if (client === null) {
    return null;
  }
  const active = deployments.data?.deployments.some((deployment) => deployment.active) ?? false;
  return (
    <Panel title="Paper deployments" meta={active ? 'Updating while a deployment runs' : 'Newest first'}>
      {deployments.status === 'pending' ? (
        <p role="status">Loading deployments…</p>
      ) : deployments.data === undefined ? (
        <Alert tone="caution" title="Deployments could not be read">
          <p>{failureText(deployments.error)}</p>
        </Alert>
      ) : (
        <>
          {deployments.isRefetchError && (
            <Alert tone="caution" title="This status may be out of date">
              <p>
                {`The latest refresh failed (${failureText(deployments.error)}). What is shown was read at ${new Date(deployments.dataUpdatedAt).toLocaleTimeString()} on this browser’s clock.`}
              </p>
            </Alert>
          )}
          {!deployments.data.enabled && (
            <Alert tone="info" title="Model deployments are disabled on this server">
              <p>
                Start the trading server with <Code>--model-deployments</Code> to create or start one. Stop and the
                kill switch work either way.
              </p>
            </Alert>
          )}
          <KillSwitch active={active} />
          {deployments.data.deployments.length === 0 ? (
            <p className={historyStyles.empty}>No model has been deployed to a paper account yet.</p>
          ) : (
            deployments.data.deployments.map((deployment) => (
              <DeploymentCard key={deployment.deployment_id} deployment={deployment} onOpenAccount={onOpenAccount} />
            ))
          )}
          <ListFooter
            shown={deployments.data.deployments.length}
            noun="deployments"
            hasMore={deployments.hasNextPage}
            loading={deployments.isFetchingNextPage}
            failed={deployments.isFetchNextPageError}
            onLoadMore={() => void deployments.fetchNextPage()}
          />
        </>
      )}
    </Panel>
  );
}
