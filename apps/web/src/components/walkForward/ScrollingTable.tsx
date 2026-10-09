import type { ReactNode } from 'react';

import historyStyles from '../history/history.module.css';

// A table wider than its panel scrolls sideways; the scrolling region is
// named and reachable with the keyboard, so its columns can be read without
// a pointer. On a phone the table is cards and nothing scrolls.
export function ScrollingTable({ label, children }: { readonly label: string; readonly children: ReactNode }) {
  return (
    <div className={historyStyles.scroll} role="group" aria-label={label} tabIndex={0}>
      {children}
    </div>
  );
}
