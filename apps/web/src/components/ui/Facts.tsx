import type { ReactNode } from 'react';

import styles from './ui.module.css';

export interface Fact {
  readonly label: string;
  readonly value: ReactNode;
}

// A labelled grid of values (a description list).
export function Facts({ facts, className }: { readonly facts: readonly Fact[]; readonly className?: string }) {
  return (
    <dl className={className ?? styles.facts}>
      {facts.map((fact) => (
        <div key={fact.label} className={styles.fact}>
          <dt className={styles.factLabel}>{fact.label}</dt>
          <dd className={styles.factValue}>{fact.value}</dd>
        </div>
      ))}
    </dl>
  );
}
