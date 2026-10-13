import { Suspense, lazy } from 'react';

import { AccountEntry } from './components/account/AccountEntry';
import { AccountContext } from './components/shell/AccountContext';
import { AppShell } from './components/shell/AppShell';
import { Workspace } from './components/workspace/Workspace';
import { useAccountSelection } from './state/accountSelection';
import { MarketDataStreamProvider } from './state/marketDataStream';
import { TRADING_HREF, useRoute } from './state/route';

// Research, with its deployment controls, loads only when it is opened, so
// the trading workspace's bundle stays small.
const Research = lazy(() =>
  import('./components/research/Research').then((module) => ({ default: module.Research })),
);
// The model library likewise, and it never changes the account selection.
const Models = lazy(() => import('./components/models/Models').then((module) => ({ default: module.Models })));
const Comparisons = lazy(() =>
  import('./components/comparisons/Comparisons').then((module) => ({ default: module.Comparisons })),
);
// Walk-forward evaluation is part of Research, loaded when opened.
const WalkForward = lazy(() =>
  import('./components/walkForward/WalkForward').then((module) => ({ default: module.WalkForward })),
);

export function App() {
  const selection = useAccountSelection();
  const route = useRoute();
  const accountId = selection.selectedAccountId;
  return (
    <MarketDataStreamProvider>
      <AppShell
        view={route.view === 'walk-forward' || route.view === 'diagnostics' ? 'research' : route.view}
        accountContext={
          accountId === null ? null : <AccountContext accountId={accountId} onSwitch={selection.closeAccount} />
        }
      >
        {route.view === 'models' ? (
          <Suspense fallback={<p role="status">Loading the model library…</p>}>
            <Models modelId={route.modelId} />
          </Suspense>
        ) : route.view === 'comparisons' ? (
          <Suspense fallback={<p role="status">Loading comparisons…</p>}>
            <Comparisons comparisonId={route.comparisonId} />
          </Suspense>
        ) : route.view === 'walk-forward' ? (
          <Suspense fallback={<p role="status">Loading walk-forward evaluation…</p>}>
            <WalkForward planId={route.planId} studyId={route.studyId} />
          </Suspense>
        ) : route.view === 'diagnostics' ? (
          <Suspense fallback={<p role="status">Loading strategy diagnostics…</p>}>
            <WalkForward
              planId={null}
              studyId={null}
              diagnostics={{ diagnosticId: route.diagnosticId, runId: route.runId, account: route.account }}
            />
          </Suspense>
        ) : route.view === 'research' ? (
          // Research changes the account selection only when asked to open
          // a deployment's account, so returning to trading otherwise finds
          // the same account open.
          <Suspense fallback={<p role="status">Loading Research…</p>}>
            <Research
              resultId={route.resultId}
              onOpenAccount={(deploymentAccount) => {
                selection.openAccount(deploymentAccount);
                window.location.hash = TRADING_HREF;
              }}
            />
          </Suspense>
        ) : accountId === null ? (
          <AccountEntry
            recentAccountIds={selection.recentAccountIds}
            onOpen={selection.openAccount}
            onForget={selection.forgetAccount}
          />
        ) : (
          // Keyed so an account's ticket state never carries over to another.
          <Workspace
            key={accountId}
            accountId={accountId}
            onClose={selection.closeAccount}
            onForget={selection.forgetAccount}
          />
        )}
      </AppShell>
    </MarketDataStreamProvider>
  );
}
