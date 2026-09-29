import type { ComponentProps } from 'react';

import { cx } from './cx';
import styles from './ui.module.css';

type Variant = 'primary' | 'secondary' | 'quiet';

const VARIANT_CLASS: Readonly<Record<Variant, string | undefined>> = {
  primary: styles.buttonPrimary,
  secondary: undefined,
  quiet: styles.buttonQuiet,
};

export function Button({
  variant = 'secondary',
  className,
  type = 'button',
  ...props
}: ComponentProps<'button'> & { readonly variant?: Variant }) {
  return <button type={type} className={cx(styles.button, VARIANT_CLASS[variant], className)} {...props} />;
}
