/** Joins class names, skipping absent ones (CSS module lookups may be undefined). */
export function cx(...classNames: readonly (string | false | null | undefined)[]): string {
  return classNames.filter(Boolean).join(' ');
}
