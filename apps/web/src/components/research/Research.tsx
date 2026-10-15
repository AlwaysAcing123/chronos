import { useState } from 'react';

import { ChronosApiError } from '../../api/errors';
import { TRADING_HREF, walkForwardHref } from '../../state/route';
import { useExperiments, useResearchClient, useResearchStatus } from '../../state/research';
import { Alert, Code } from '../ui/Alert';
import { Button } from '../ui/Button';
import { Panel } from '../ui/Panel';
import workspaceStyles from '../workspace/workspace.module.css';
import { DeploymentForm } from './DeploymentForm';
import { DeploymentList } from './DeploymentList';
import { EvaluationForm } from './EvaluationForm';
import { ModelCatalog, defaultSelection } from './ModelCatalog';
import type { Selection } from './ModelCatalog';
import styles from './research.module.css';
import { ResultList } from './ResultList';
import { ResultView } from './ResultView';
import { RunList } from './RunList';

const START_COMMAND =
  'python -m chronos_research serve --tools <build> --artifacts ../data --state ../data/research';

// A failed read from the research service, with what it means.
export function ResearchFailure({
  error,
  title = 'The research service could not answer',
  onRetry,
  retrying = false,
}: {
  readonly error: unknown;
  readonly title?: string;
  readonly onRetry?: () => void;
  readonly retrying?: boolean;
}) {
  const failure = error instanceof ChronosApiError ? error.failure : null;
  const unreachable = failure?.kind === 'network';
  return (
    <Alert
      tone={unreachable ? 'caution' : 'negative'}
      title={unreachable ? 'The research service is not reachable' : title}
      actions={
        onRetry && (
          <Button onClick={onRetry} disabled={retrying}>
            {retrying ? 'Retrying…' : 'Retry'}
          </Button>
        )
      }
    >
      {unreachable ? (
        <p>
          Start it beside the trading server, for example <Code>{START_COMMAND}</Code>. Trading is unaffected.
        </p>
      ) : failure?.kind === 'api_error' ? (
        <p>
          {failure.error.message} <Code>{failure.error.reason ?? failure.error.code}</Code>
        </p>
      ) : failure?.kind === 'unexpected_response' ? (
        <p>{failure.detail}</p>
      ) : (
        <p>{error instanceof Error ? error.message : 'Unknown failure'}</p>
      )}
    </Alert>
  );
}

function Heading({ title, note }: { readonly title: string; readonly note: string }) {
  return (
    <div className={workspaceStyles.heading}>
      <div>
        <p className={workspaceStyles.eyebrow}>Research</p>
        <h1 className={workspaceStyles.title}>{title}</h1>
      </div>
      <p className={workspaceStyles.workspaceNote}>{note}</p>
    </div>
  );
}

function ResearchHome({ onOpenAccount }: { readonly onOpenAccount: ((accountId: string) => void) | undefined }) {
  const status = useResearchStatus();
  const experiments = useExperiments();
  const [chosen, setSelection] = useState<Selection | null>(null);
  const selection = chosen ?? defaultSelection(experiments.data);

  if (status.status === 'pending') {
    return (
      <Panel title="Research service">
        <p role="status">Connecting to the research service…</p>
      </Panel>
    );
  }
  if (status.status === 'error') {
    return (
      <Panel title="Research service">
        <ResearchFailure error={status.error} onRetry={() => void status.refetch()} retrying={status.isFetching} />
      </Panel>
    );
  }
  const missing = Object.entries(status.data.tools).filter(([, state]) => state === 'missing');
  return (
    <div className={styles.grid}>
      <div className={styles.column}>
        {missing.length > 0 && (
          <Alert tone="caution" title="The research service is missing built tools">
            <p>
              Evaluations will fail until these are built: {missing.map(([name]) => name).join(', ')}.
            </p>
          </Alert>
        )}
        <ModelCatalog experiments={experiments} selection={selection} onSelect={setSelection} />
        <div className={styles.formPanel}>
          <EvaluationForm selection={selection} defaults={status.data.defaults} experiments={experiments.data} />
        </div>
        <div className={styles.formPanel}>
          <DeploymentForm selection={selection} defaults={status.data.defaults} experiments={experiments.data} />
        </div>
      </div>
      <div className={styles.column}>
        <DeploymentList onOpenAccount={onOpenAccount} />
        <RunList />
        <ResultList />
      </div>
    </div>
  );
}

export function Research({
  resultId,
  onOpenAccount,
}: {
  readonly resultId: string | null;
  // Opens a deployment's account in the trading workspace, on request.
  readonly onOpenAccount?: (accountId: string) => void;
}) {
  const client = useResearchClient();
  return (
    <div className={styles.research}>
      {resultId === null ? (
        <Heading title="Historical evaluation" note="Model-driven paper simulation on a dataset’s quotes" />
      ) : (
        <Heading title="Evaluation result" note="Paper simulation on a dataset’s quotes" />
      )}
      <nav aria-label="Research navigation" className={styles.inline}>
        {resultId !== null && (
          <a className={styles.back} href="#/research">
            ← All research
          </a>
        )}
        {resultId === null && (
          <a className={styles.back} href={walkForwardHref()}>
            Walk-forward evaluation
          </a>
        )}
        <a className={styles.back} href={TRADING_HREF}>
          Back to trading
        </a>
      </nav>
      <p className={styles.note}>
        Every evaluation here is a historical simulation in its own fresh accounts; it never touches a paper account.
        A deployment trades a verified model on the server&rsquo;s market data, in a new paper account of its own, only
        after you start it.
      </p>
      {client === null ? (
        <Alert tone="caution" title="Research is not configured">
          <p>This page was built without a research service client.</p>
        </Alert>
      ) : resultId === null ? (
        <ResearchHome onOpenAccount={onOpenAccount} />
      ) : (
        <ResultView runId={resultId} />
      )}
    </div>
  );
}
