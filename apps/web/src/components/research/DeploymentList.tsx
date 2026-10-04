import { useId, useState } from 'react';

import type { Decision, Deployment, DeploymentState } from '../../api/deployments';
import { isUncertainDeploymentControl } from '../../api/deployments';
import { ChronosApiError } from '../../api/errors';
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

// Exact, from the integer string: 1234 µs is "1.234".
function microsecondsAsMilliseconds(microseconds: string): string {
  const digits = microseconds.padStart(4, '0');
  const fraction = digits.slice(-3).replace(/0+$/, '');
  return `${digits.slice(0, -3)}${fraction === '' ? '' : `.${fraction}`}`;
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

function DecisionRows({ deployment }: { readonly deployment: Deployment }) {
  const decisions = useDecisions(deployment.deployment_id, deployment.active);
  if (decisions.status === 'pending') {
    return <p role="status">Loading decisions…</p>;
  }
  if (decisions.data === undefined) {
    return <p className={historyStyles.empty}>Decisions could not be read: {failureText(decisions.error)}</p>;
  }
  const rows: readonly Decision[] = decisions.data.decisions.slice(0, 10);
  if (rows.length === 0) {
    return <p className={historyStyles.empty}>No decisions yet.</p>;
  }
  return (
    <div className={historyStyles.scroll}>
      <table className={historyStyles.table}>
        <caption className="visually-hidden">Latest decisions of {deployment.deployment_id}, newest first</caption>
        <thead>
          <tr>
            <th scope="col">Observation (UTC)</th>
            <th scope="col">Prediction</th>
            <th scope="col">Decision</th>
            <th scope="col">Outcome</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((decision) => (
            <tr key={decision.decision_sequence}>
              <td data-label="Observation (UTC)">
                <MarketTime value={decision.observation.received_at} stacked />
              </td>
              <td data-label="Prediction">
                <div>
                  {decision.prediction.class}
                  <span className={styles.reasonCode}>{decision.prediction.confidence}</span>
                </div>
              </td>
              <td data-label="Decision">
                <div>
                  {decision.order === null ? 'Hold' : `${decision.order.side} ${decision.order.quantity}`}
                  <span className={styles.reasonCode}>{decision.policy_reason}</span>
                </div>
              </td>
              <td className={historyStyles.detail} data-label="Outcome">
                <div>
                  {decision.state}
                  {decision.resolution !== null && <span className={styles.reasonCode}>{decision.resolution}</span>}
                </div>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
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
  return (
    <article className={styles.section} aria-label={`Deployment ${deployment.deployment_id}`}>
      <div className={styles.inline}>
        <h3 className={styles.sectionTitle}>{deployment.deployment_id}</h3>
        <Badge tone={state.tone}>{state.label}</Badge>
        <span className={styles.choiceDetail}>{REASON[deployment.state_reason] ?? deployment.state_reason}</span>
      </div>
      <Facts
        facts={[
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
          {
            label: 'Model',
            value: `${deployment.model.instrument} · ${MODEL_LABEL[deployment.model.name]} · ${shortId(deployment.model.experiment_id)}`,
          },
          { label: 'Parameters', value: <Code>{shortId(deployment.model.parameters_sha256)}</Code> },
          { label: 'Training cutoff (UTC)', value: <MarketTime value={deployment.model.training_cutoff} /> },
          {
            label: 'Policy',
            value: `Threshold ${deployment.policy.threshold} · target ${deployment.policy.target_quantity}`,
          },
          {
            label: 'Timing',
            value: `${deployment.timing.latency_ms} ms latency · ${deployment.timing.decision_interval_ms} ms between decisions · ${deployment.timing.max_signal_age_ms} ms max signal age`,
          },
          {
            label: 'This run',
            value:
              run === null
                ? `Run ${deployment.run_generation}`
                : `Run ${deployment.run_generation}: ${run.predictions} predictions, ${run.order_decisions} order decisions, ${run.orders_recorded} orders, ${run.abandoned} abandoned, ${run.superseded} superseded`,
          },
          ...(run === null
            ? []
            : [
                {
                  label: 'Observed timing (wall clock)',
                  value: `Inference up to ${microsecondsAsMilliseconds(run.max_inference_us)} ms · order path up to ${microsecondsAsMilliseconds(run.max_submission_us)} ms`,
                },
              ]),
          {
            label: 'Latest decided observation',
            value:
              deployment.watermark === null ? 'None yet' : <MarketTime value={deployment.watermark.received_at} />,
          },
        ]}
      />
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
      <DecisionRows deployment={deployment} />
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
