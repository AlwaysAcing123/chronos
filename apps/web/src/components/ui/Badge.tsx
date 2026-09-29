import type { Tone } from '../../presentation/vocabulary';
import { cx } from './cx';
import styles from './ui.module.css';

export function Badge({ tone, children }: { readonly tone: Tone; readonly children: string }) {
  return <span className={cx(styles.badge, styles[`tone-${tone}`])}>{children}</span>;
}
