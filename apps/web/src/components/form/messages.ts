import type { DecimalInputProblem } from '../../api/decimal';

export function decimalProblemMessage(problem: DecimalInputProblem, fractionDigits: number): string {
  switch (problem) {
    case 'required':
      return 'Required.';
    case 'invalid_format':
      return 'Enter a plain decimal such as 10 or 10.5, without signs, spaces, separators, or exponents.';
    case 'too_many_fractional_digits':
      return `Use at most ${String(fractionDigits)} decimal places.`;
    case 'out_of_range':
      return 'This value exceeds the supported range.';
    case 'not_positive':
      return 'Must be greater than zero.';
  }
}

export const IDENTIFIER_RULE =
  '1–64 characters: letters, digits, ".", "_" or "-", starting with a letter or digit.';
