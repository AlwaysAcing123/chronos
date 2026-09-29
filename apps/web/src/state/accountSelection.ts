// Browser-persisted conveniences: which account is open and which IDs were
// used recently (and, in instrumentSelection.ts, the symbol on screen).
// Balances, positions, and orders are always re-read from the server.
import { useCallback, useEffect, useState } from 'react';

import { isValidIdentifier } from '../api/identifiers';

export const SELECTED_ACCOUNT_KEY = 'chronos.web.v1.selected-account';
export const RECENT_ACCOUNTS_KEY = 'chronos.web.v1.recent-accounts';
const MAXIMUM_RECENT_ACCOUNTS = 6;

export interface AccountSelection {
  readonly selectedAccountId: string | null;
  readonly recentAccountIds: readonly string[];
}

// Storage can be disabled or full; selection then lasts only for the page.
function browserStorage(): Storage | null {
  try {
    return window.localStorage;
  } catch {
    return null;
  }
}

function readRecent(storage: Storage): string[] {
  try {
    const parsed: unknown = JSON.parse(storage.getItem(RECENT_ACCOUNTS_KEY) ?? '[]');
    return Array.isArray(parsed)
      ? parsed.filter((id): id is string => typeof id === 'string' && isValidIdentifier(id))
          .slice(0, MAXIMUM_RECENT_ACCOUNTS)
      : [];
  } catch {
    return [];
  }
}

export function loadAccountSelection(storage: Storage | null = browserStorage()): AccountSelection {
  if (storage === null) {
    return { selectedAccountId: null, recentAccountIds: [] };
  }
  try {
    const selected = storage.getItem(SELECTED_ACCOUNT_KEY);
    return {
      selectedAccountId: selected !== null && isValidIdentifier(selected) ? selected : null,
      recentAccountIds: readRecent(storage),
    };
  } catch {
    return { selectedAccountId: null, recentAccountIds: [] };
  }
}

function saveAccountSelection(selection: AccountSelection, storage: Storage | null = browserStorage()) {
  if (storage === null) {
    return;
  }
  try {
    if (selection.selectedAccountId === null) {
      storage.removeItem(SELECTED_ACCOUNT_KEY);
    } else {
      storage.setItem(SELECTED_ACCOUNT_KEY, selection.selectedAccountId);
    }
    storage.setItem(RECENT_ACCOUNTS_KEY, JSON.stringify(selection.recentAccountIds));
  } catch {
    // Persistence is a convenience; the in-memory selection still applies.
  }
}

export function useAccountSelection() {
  const [selection, setSelection] = useState<AccountSelection>(() => loadAccountSelection());

  useEffect(() => {
    saveAccountSelection(selection);
  }, [selection]);

  const openAccount = useCallback((accountId: string) => {
    setSelection((current) => ({
      selectedAccountId: accountId,
      recentAccountIds: [accountId, ...current.recentAccountIds.filter((id) => id !== accountId)].slice(
        0,
        MAXIMUM_RECENT_ACCOUNTS,
      ),
    }));
  }, []);

  const closeAccount = useCallback(() => {
    setSelection((current) => ({ ...current, selectedAccountId: null }));
  }, []);

  const forgetAccount = useCallback((accountId: string) => {
    setSelection((current) => ({
      selectedAccountId: current.selectedAccountId === accountId ? null : current.selectedAccountId,
      recentAccountIds: current.recentAccountIds.filter((id) => id !== accountId),
    }));
  }, []);

  return { ...selection, openAccount, closeAccount, forgetAccount };
}
