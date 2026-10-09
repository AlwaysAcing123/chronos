import { TRADING_HREF, researchHref, walkForwardHref } from '../../state/route';
import { useResearchClient, useResearchStatus } from '../../state/research';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert } from '../ui/Alert';
import { Panel } from '../ui/Panel';
import workspaceStyles from '../workspace/workspace.module.css';
import { CollectionList } from './CollectionList';
import { PlanReview } from './PlanReview';
import { StudyDetailView } from './StudyDetail';
import { StudyJobs } from './StudyJobs';
import { StudyList } from './StudyList';
import { WalkForwardForm } from './WalkForwardForm';

function WalkForwardHome() {
  const status = useResearchStatus();
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
        <CollectionList />
        <StudyList />
      </div>
      <div className={styles.column}>
        <div className={styles.formPanel}>
          <WalkForwardForm defaults={status.data.defaults} />
        </div>
        <StudyJobs />
      </div>
    </div>
  );
}

// Walk-forward evaluation, inside Research: fresh models fitted on earlier
// independent recordings and measured on each later one.
export function WalkForward({ planId, studyId }: { readonly planId: string | null; readonly studyId: string | null }) {
  const client = useResearchClient();
  const title = studyId !== null ? 'Walk-forward study' : planId !== null ? 'Review the plan' : 'Walk-forward evaluation';
  return (
    <div className={styles.research}>
      <div className={workspaceStyles.heading}>
        <div>
          <p className={workspaceStyles.eyebrow}>Research</p>
          <h1 className={workspaceStyles.title}>{title}</h1>
        </div>
        <p className={workspaceStyles.workspaceNote}>Fresh models per fold, measured on later independent recordings</p>
      </div>
      <nav aria-label="Walk-forward navigation" className={styles.inline}>
        {(planId !== null || studyId !== null) && (
          <a className={styles.back} href={walkForwardHref()}>
            ← All walk-forward studies
          </a>
        )}
        <a className={styles.back} href={researchHref()}>
          Historical evaluation
        </a>
        <a className={styles.back} href={TRADING_HREF}>
          Back to trading
        </a>
      </nav>
      <p className={styles.note}>
        Each fold fits a fresh majority model and a fresh logistic model on earlier recordings only, predicts every row of
        the next recording, and runs those predictions through the C++ engine once per cost and latency scenario, each in
        its own new accounts. Nothing is promoted or deployed, and no paper trading account is touched.
      </p>
      {client === null ? (
        <Alert tone="caution" title="Research is not configured">
          <p>This page was built without a research service client.</p>
        </Alert>
      ) : studyId !== null ? (
        <StudyDetailView studyId={studyId} />
      ) : planId !== null ? (
        <PlanReview planId={planId} />
      ) : (
        <WalkForwardHome />
      )}
    </div>
  );
}
