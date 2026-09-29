import type { ReactNode } from 'react';

import type { Tone } from '../../presentation/vocabulary';
import { cx } from './cx';
import styles from './ui.module.css';

// `live` makes screen readers announce changes; use it for outcomes the user
// just caused, not for every polled refresh.
export function Alert({
  tone,
  title,
  children,
  actions,
  live = false,
}: {
  readonly tone: Tone;
  readonly title: string;
  readonly children?: ReactNode;
  readonly actions?: ReactNode;
  readonly live?: boolean;
}) {
  return (
    <div className={cx(styles.alert, styles[`tone-${tone}`])} role={live ? 'status' : undefined}>
      <p className={styles.alertTitle}>{title}</p>
      {children !== undefined && <div className={styles.alertBody}>{children}</div>}
      {actions !== undefined && <div className={styles.alertActions}>{actions}</div>}
    </div>
  );
}

export function Code({ children }: { readonly children: string }) {
  return <code className={styles.code}>{children}</code>;
}
