// The symbol the workspace shows in its watchlist and order ticket. Like the
// account selection it is a per-browser convenience: it survives reloads and
// refreshes, but the symbols themselves always come from the server.
import { useCallback, useState } from 'react';

import { isValidIdentifier } from '../api/identifiers';

export const SELECTED_INSTRUMENT_KEY = 'chronos.web.v1.selected-instrument';

function browserStorage(): Storage | null {
  try {
    return window.localStorage;
  } catch {
    return null;
  }
}

export function loadSelectedInstrument(storage: Storage | null = browserStorage()): string | null {
  try {
    const stored = storage?.getItem(SELECTED_INSTRUMENT_KEY) ?? null;
    return stored !== null && isValidIdentifier(stored) ? stored : null;
  } catch {
    return null;
  }
}

function saveSelectedInstrument(instrument: string, storage: Storage | null = browserStorage()) {
  try {
    storage?.setItem(SELECTED_INSTRUMENT_KEY, instrument);
  } catch {
    // Persistence is a convenience; the in-memory selection still applies.
  }
}

/**
 * The selected symbol among `instruments`, in server configuration order. A
 * remembered symbol the server no longer offers falls back to the first one;
 * null only while no symbols are known.
 */
export function useSelectedInstrument(instruments: readonly string[]) {
  const [remembered, setRemembered] = useState<string | null>(() => loadSelectedInstrument());
  const selected =
    remembered !== null && instruments.includes(remembered) ? remembered : (instruments[0] ?? null);
  const select = useCallback((instrument: string) => {
    setRemembered(instrument);
    saveSelectedInstrument(instrument);
  }, []);
  return { selected, select };
}
