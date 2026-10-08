import { modelsHref, researchHref } from '../../state/route';
import { useExperiments, useResearchClient, useResearchStatus } from '../../state/research';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert } from '../ui/Alert';
import { Panel } from '../ui/Panel';
import workspaceStyles from '../workspace/workspace.module.css';
import { ComparisonDetailView } from './ComparisonDetail';
import { ComparisonForm } from './ComparisonForm';
import { ComparisonList } from './ComparisonList';

function ComparisonsHome() {
  const status = useResearchStatus();
  const experiments = useExperiments();
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
  return (
    <div className={styles.grid}>
      <div className={styles.column}>
        <ComparisonList />
      </div>
      <div className={styles.column}>
        <div className={styles.formPanel}>
          <ComparisonForm experiments={experiments.data} defaults={status.data.defaults} />
        </div>
      </div>
    </div>
  );
}

export function Comparisons({ comparisonId }: { readonly comparisonId: string | null }) {
  const client = useResearchClient();
  return (
    <div className={styles.research}>
      <div className={workspaceStyles.heading}>
        <div>
          <p className={workspaceStyles.eyebrow}>Comparisons</p>
          <h1 className={workspaceStyles.title}>{comparisonId === null ? 'Model comparisons' : 'Comparison'}</h1>
        </div>
        <p className={workspaceStyles.workspaceNote}>Registered model versions under one frozen specification</p>
      </div>
      <nav aria-label="Comparison navigation" className={styles.inline}>
        {comparisonId !== null && (
          <a className={styles.back} href="#/comparisons">
            ← All comparisons
          </a>
        )}
        <a className={styles.back} href={modelsHref()}>
          Model library
        </a>
        <a className={styles.back} href={researchHref()}>
          Historical evaluation
        </a>
      </nav>
      <p className={styles.note}>
        A comparison predicts every member on the same rows of the same verified split, runs each in its own paper
        accounts beside cash and buy-and-hold, and replays every run. Nothing is retrained and no paper trading account
        is touched.
      </p>
      {client === null ? (
        <Alert tone="caution" title="Research is not configured">
          <p>This page was built without a research service client.</p>
        </Alert>
      ) : comparisonId === null ? (
        <ComparisonsHome />
      ) : (
        <ComparisonDetailView comparisonId={comparisonId} />
      )}
    </div>
  );
}
