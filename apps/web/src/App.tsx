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

export function App() {
  const selection = useAccountSelection();
  const route = useRoute();
  const accountId = selection.selectedAccountId;
  return (
    <MarketDataStreamProvider>
      <AppShell
        view={route.view}
        accountContext={
          accountId === null ? null : <AccountContext accountId={accountId} onSwitch={selection.closeAccount} />
        }
      >
        {route.view === 'research' ? (
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
