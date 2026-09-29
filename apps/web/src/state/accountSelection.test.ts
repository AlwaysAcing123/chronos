import { act, renderHook } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { loadAccountSelection, RECENT_ACCOUNTS_KEY, SELECTED_ACCOUNT_KEY, useAccountSelection } from './accountSelection';

describe('account selection', () => {
  it('remembers the selected account and the most recent first', () => {
    const { result } = renderHook(() => useAccountSelection());
    act(() => {
      result.current.openAccount('alpha');
    });
    act(() => {
      result.current.openAccount('beta');
    });
    act(() => {
      result.current.openAccount('alpha');
    });
    expect(result.current.selectedAccountId).toBe('alpha');
    expect(result.current.recentAccountIds).toEqual(['alpha', 'beta']);
    expect(loadAccountSelection()).toEqual({ selectedAccountId: 'alpha', recentAccountIds: ['alpha', 'beta'] });
  });

  it('keeps at most six recent accounts', () => {
    const { result } = renderHook(() => useAccountSelection());
    for (const id of ['a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7']) {
      act(() => {
        result.current.openAccount(id);
      });
    }
    expect(result.current.recentAccountIds).toEqual(['a7', 'a6', 'a5', 'a4', 'a3', 'a2']);
  });

  it('ignores corrupt or invalid stored values', () => {
    window.localStorage.setItem(SELECTED_ACCOUNT_KEY, 'bad id');
    window.localStorage.setItem(RECENT_ACCOUNTS_KEY, '{not json');
    expect(loadAccountSelection()).toEqual({ selectedAccountId: null, recentAccountIds: [] });
    window.localStorage.setItem(RECENT_ACCOUNTS_KEY, JSON.stringify(['ok-1', 42, '../x', 'ok-2']));
    expect(loadAccountSelection().recentAccountIds).toEqual(['ok-1', 'ok-2']);
  });

  it('works without storage', () => {
    expect(loadAccountSelection(null)).toEqual({ selectedAccountId: null, recentAccountIds: [] });
  });
});
