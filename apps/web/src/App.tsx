import { AccountEntry } from './components/account/AccountEntry';
import { AccountContext } from './components/shell/AccountContext';
import { AppShell } from './components/shell/AppShell';
import { Workspace } from './components/workspace/Workspace';
import { useAccountSelection } from './state/accountSelection';

export function App() {
  const selection = useAccountSelection();
  const accountId = selection.selectedAccountId;
  return (
    <AppShell
      accountContext={accountId === null ? null : <AccountContext accountId={accountId} onSwitch={selection.closeAccount} />}
    >
      {accountId === null ? (
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
  );
}
