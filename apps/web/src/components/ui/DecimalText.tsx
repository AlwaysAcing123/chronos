import type { DiagnosticDecimal } from '../../api/diagnosticsSchemas';
import type { ExactDecimal } from '../../api/schemas';
import { decimalSign } from '../../api/decimal';
import { cx } from './cx';
import styles from './ui.module.css';

// Renders the server's exact decimal string, including the diagnostics' own
// 18-place figures. `signed` colours gains and losses; the minus sign still
// carries the meaning in text.
export function DecimalText({
  value,
  signed = false,
  suffix,
}: {
  readonly value: ExactDecimal | DiagnosticDecimal;
  readonly signed?: boolean;
  readonly suffix?: string;
}) {
  const sign = decimalSign(value);
  return (
    <span className={cx('numeric', signed && sign !== 'zero' && styles[`sign-${sign}`])}>
      {value}
      {suffix}
    </span>
  );
}

/** A value the server could not provide. It is never shown as zero. */
export function Unavailable({ children = 'Unavailable' }: { readonly children?: string }) {
  return <span className={styles.unavailable}>{children}</span>;
}
