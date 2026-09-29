import type { ReactNode } from 'react';
import { useId } from 'react';

import { cx } from './cx';
import styles from './ui.module.css';

export function Panel({
  title,
  meta,
  id,
  className,
  children,
}: {
  readonly title: string;
  readonly meta?: ReactNode;
  readonly id?: string | undefined;
  readonly className?: string | undefined;
  readonly children: ReactNode;
}) {
  const headingId = useId();
  return (
    <section id={id} aria-labelledby={headingId} className={cx(styles.panel, className)}>
      <div className={styles.panelHeader}>
        <h2 id={headingId} className={styles.panelTitle}>
          {title}
        </h2>
        {meta !== undefined && <div className={styles.panelMeta}>{meta}</div>}
      </div>
      <div className={styles.panelBody}>{children}</div>
    </section>
  );
}
