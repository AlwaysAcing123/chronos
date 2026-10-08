import type { ReactNode } from 'react';

import { TRADING_HREF, comparisonsHref, modelsHref, researchHref } from '../../state/route';
import { SourceBar } from '../source/SourceBar';
import { ConnectionStatus } from './ConnectionStatus';
import { PaperBanner } from './PaperBanner';
import styles from './shell.module.css';

export type View = 'trading' | 'models' | 'comparisons' | 'research';

const VIEWS: readonly { readonly view: View; readonly label: string; readonly href: string }[] = [
  { view: 'trading', label: 'Trading', href: TRADING_HREF },
  { view: 'models', label: 'Models', href: modelsHref() },
  { view: 'comparisons', label: 'Comparisons', href: comparisonsHref() },
  { view: 'research', label: 'Research', href: researchHref() },
];

export function AppShell({
  accountContext,
  view = 'trading',
  children,
}: {
  readonly accountContext: ReactNode;
  readonly view?: View;
  readonly children: ReactNode;
}) {
  return (
    <div className={styles.app}>
      <a className={styles.skipLink} href="#main">
        Skip to content
      </a>
      <PaperBanner />
      <header className={styles.header}>
        <div className={styles.brand}>
          <svg className={styles.brandMark} viewBox="0 0 32 32" aria-hidden="true">
            <rect width="32" height="32" rx="3" />
            <path d="M23 10a9 9 0 1 0 0 12M16 10v6l4 3" fill="none" strokeWidth="2" strokeLinecap="square" />
          </svg>
          Chronos
        </div>
        <nav className={styles.primaryNav} aria-label="Primary">
          {VIEWS.map((item) => (
            <a key={item.view} href={item.href} aria-current={item.view === view ? 'page' : undefined}>
              {item.label}
            </a>
          ))}
        </nav>
        {accountContext}
        <ConnectionStatus />
      </header>
      <SourceBar />
      <main id="main" className={styles.main} tabIndex={-1}>
        {children}
      </main>
    </div>
  );
}
