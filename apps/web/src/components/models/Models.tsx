import { researchHref } from '../../state/route';
import { useExperiments, useResearchClient, useResearchStatus } from '../../state/research';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert } from '../ui/Alert';
import { Panel } from '../ui/Panel';
import workspaceStyles from '../workspace/workspace.module.css';
import { ImportModelForm } from './ImportModelForm';
import { ModelDetailView } from './ModelDetail';
import { ModelLibrary } from './ModelLibrary';
import { RegisterModelForm } from './RegisterModelForm';

function LibraryHome() {
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
        <ModelLibrary />
      </div>
      <div className={styles.column}>
        <div className={styles.formPanel}>
          <RegisterModelForm experiments={experiments.data} />
        </div>
        <div className={styles.formPanel}>
          <ImportModelForm status={status.data} />
        </div>
      </div>
    </div>
  );
}

export function Models({ modelId }: { readonly modelId: string | null }) {
  const client = useResearchClient();
  return (
    <div className={styles.research}>
      <div className={workspaceStyles.heading}>
        <div>
          <p className={workspaceStyles.eyebrow}>Models</p>
          <h1 className={workspaceStyles.title}>{modelId === null ? 'Model library' : 'Model version'}</h1>
        </div>
        <p className={workspaceStyles.workspaceNote}>Immutable model versions, verified whenever they are read</p>
      </div>
      <nav aria-label="Model navigation" className={styles.inline}>
        {modelId !== null && (
          <a className={styles.back} href="#/models">
            ← All models
          </a>
        )}
        <a className={styles.back} href={researchHref()}>
          Historical evaluation
        </a>
      </nav>
      <p className={styles.note}>
        A model version names the exact bytes a prediction comes from, with its inputs, outputs, and where it came from.
        Registering or importing one trains nothing, changes no experiment, and touches no paper account.
      </p>
      {client === null ? (
        <Alert tone="caution" title="Research is not configured">
          <p>This page was built without a research service client.</p>
        </Alert>
      ) : modelId === null ? (
        <LibraryHome />
      ) : (
        <ModelDetailView modelId={modelId} />
      )}
    </div>
  );
}
