import type { InputHTMLAttributes, ReactNode } from 'react';
import { useId } from 'react';

import styles from './form.module.css';

// A labelled input whose hint and error are announced with it.
export function TextField({
  label,
  hint,
  error,
  ...input
}: InputHTMLAttributes<HTMLInputElement> & {
  readonly label: string;
  readonly hint?: ReactNode;
  readonly error?: string | null | undefined;
}) {
  const id = useId();
  const hintId = `${id}-hint`;
  const errorId = `${id}-error`;
  const describedBy = [hint !== undefined && hintId, error && errorId].filter(Boolean).join(' ');
  return (
    <div className={styles.field}>
      <label className={styles.label} htmlFor={id}>
        {label}
      </label>
      <input
        id={id}
        className={styles.input}
        aria-invalid={error ? true : undefined}
        aria-describedby={describedBy === '' ? undefined : describedBy}
        {...input}
      />
      {hint !== undefined && (
        <p id={hintId} className={styles.hint}>
          {hint}
        </p>
      )}
      {error && (
        <p id={errorId} className={styles.error}>
          {error}
        </p>
      )}
    </div>
  );
}

// A segmented single choice built from native radio buttons, so arrow keys
// and screen readers behave as for any radio group.
export function Choice<Value extends string>({
  legend,
  name,
  value,
  options,
  onChange,
  disabled = false,
}: {
  readonly legend: string;
  readonly name: string;
  readonly value: Value;
  readonly options: readonly { readonly value: Value; readonly label: string }[];
  readonly onChange: (value: Value) => void;
  readonly disabled?: boolean;
}) {
  return (
    <fieldset className={styles.choice} disabled={disabled}>
      <legend className={styles.choiceLegend}>{legend}</legend>
      <div className={styles.choiceOptions}>
        {options.map((option) => (
          <label key={option.value} className={styles.choiceOption}>
            <input
              type="radio"
              name={name}
              value={option.value}
              checked={value === option.value}
              onChange={() => {
                onChange(option.value);
              }}
            />
            <span>{option.label}</span>
          </label>
        ))}
      </div>
    </fieldset>
  );
}
