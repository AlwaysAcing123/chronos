import { AccountEntry } from './components/account/AccountEntry';
import { Research } from './components/research/Research';
import { AccountContext } from './components/shell/AccountContext';
import { AppShell } from './components/shell/AppShell';
import { Workspace } from './components/workspace/Workspace';
import { useAccountSelection } from './state/accountSelection';
import { MarketDataStreamProvider } from './state/marketDataStream';
import { useRoute } from './state/route';

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
          // Research never reads or changes the account selection, so
          // returning to trading finds the same account open.
          <Research resultId={route.resultId} />
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
