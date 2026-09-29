import type { ReactNode } from 'react';

import { ConnectionStatus } from './ConnectionStatus';
import { PaperBanner } from './PaperBanner';
import styles from './shell.module.css';

export function AppShell({
  accountContext,
  children,
}: {
  readonly accountContext: ReactNode;
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
            <rect width="32" height="32" rx="6" fill="#1a1e24" />
            <path d="M21.5 11.2A7 7 0 1 0 21.5 20.8" fill="none" stroke="#7fa7e8" strokeWidth="3" strokeLinecap="round" />
          </svg>
          Chronos
        </div>
        {accountContext}
        <ConnectionStatus />
      </header>
      <main id="main" className={styles.main} tabIndex={-1}>
        {children}
      </main>
    </div>
  );
}
