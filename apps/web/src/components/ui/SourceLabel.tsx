import type { OrderSource } from '../../api/schemas';
import { SOURCE_KIND } from '../../presentation/vocabulary';
import { cx } from './cx';
import styles from './ui.module.css';

// Manual and model orders share one execution path; only the attribution differs.
export function SourceLabel({ source }: { readonly source: OrderSource }) {
  return (
    <span className={cx(styles.source, styles[`source-${source.kind}`])}>
      <span className={styles.sourceKind}>{SOURCE_KIND[source.kind]}</span>
      <span className={cx(styles.sourceId, 'mono')}>{source.id}</span>
    </span>
  );
}
