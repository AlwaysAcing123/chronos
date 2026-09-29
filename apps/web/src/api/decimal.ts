// Lexical rules for the API's exact decimal strings. Nothing here converts a
// decimal to a JavaScript number: the browser validates and displays decimal
// text, and the C++ server performs every calculation.

export type DecimalInputKind = 'quantity' | 'price' | 'money';

// Fractional digits each fixed-point type represents (docs/paper-trading-api.md).
export const FRACTION_DIGITS = {
  quantity: 8,
  price: 9,
  money: 8,
  percentage: 8,
} as const;

// The request grammar the server accepts.
const REQUEST_DECIMAL = /^-?(0|[1-9][0-9]*)(\.[0-9]+)?$/;
// The canonical form the server renders: no trailing fractional zeros and no
// negative zero.
const CANONICAL_DECIMAL = /^-?(0|[1-9][0-9]*)(\.[0-9]*[1-9])?$/;
// Every fixed-point value is a signed 64-bit count of its smallest unit.
const MAXIMUM_RAW_UNITS = '9223372036854775807';

export type DecimalInputProblem =
  | 'required'
  | 'invalid_format'
  | 'too_many_fractional_digits'
  | 'out_of_range'
  | 'not_positive';

export type DecimalInputResult =
  | { readonly ok: true; readonly value: string }
  | { readonly ok: false; readonly problem: DecimalInputProblem };

function fractionDigits(text: string): string {
  const point = text.indexOf('.');
  return point === -1 ? '' : text.slice(point + 1);
}

function integerDigits(text: string): string {
  const unsigned = text.startsWith('-') ? text.slice(1) : text;
  const point = unsigned.indexOf('.');
  return point === -1 ? unsigned : unsigned.slice(0, point);
}

function isZero(text: string): boolean {
  return /^-?0(\.0+)?$/.test(text);
}

// Whether a positive decimal with at most `scale` fractional digits fits the
// fixed-point range, compared as digit strings.
function fitsFixedPointRange(text: string, scale: number): boolean {
  const rawUnits = (integerDigits(text) + fractionDigits(text).padEnd(scale, '0'))
    .replace(/^0+(?=\d)/, '');
  if (rawUnits.length !== MAXIMUM_RAW_UNITS.length) {
    return rawUnits.length < MAXIMUM_RAW_UNITS.length;
  }
  return rawUnits <= MAXIMUM_RAW_UNITS;
}

/**
 * Checks what a user typed for a positive quantity, price, or cash amount.
 * The accepted text is returned unchanged, so the request carries exactly
 * what the user reviewed; the server remains the authority.
 */
export function validatePositiveDecimal(
  text: string,
  kind: DecimalInputKind,
): DecimalInputResult {
  if (text === '') {
    return { ok: false, problem: 'required' };
  }
  if (!REQUEST_DECIMAL.test(text)) {
    return { ok: false, problem: 'invalid_format' };
  }
  if (text.startsWith('-') || isZero(text)) {
    return { ok: false, problem: 'not_positive' };
  }
  const scale = FRACTION_DIGITS[kind];
  if (fractionDigits(text).length > scale) {
    return { ok: false, problem: 'too_many_fractional_digits' };
  }
  if (!fitsFixedPointRange(text, scale)) {
    return { ok: false, problem: 'out_of_range' };
  }
  return { ok: true, value: text };
}

/** Whether `text` is a canonical response decimal with at most `scale` fractional digits. */
export function isCanonicalDecimal(text: string, scale: number): boolean {
  return (
    CANONICAL_DECIMAL.test(text) &&
    text !== '-0' &&
    fractionDigits(text).length <= scale
  );
}

export type DecimalSign = 'negative' | 'zero' | 'positive';

/** The sign of a canonical decimal, read from its text. */
export function decimalSign(canonical: string): DecimalSign {
  if (canonical.startsWith('-')) {
    return 'negative';
  }
  return canonical === '0' ? 'zero' : 'positive';
}

/**
 * Whether two valid request or response decimals denote the same value, by
 * removing trailing fractional zeros. Used only to recognise a recorded order
 * as the one this client sent; values sent to the server are never rewritten.
 */
export function sameDecimal(left: string, right: string): boolean {
  return withoutTrailingZeros(left) === withoutTrailingZeros(right);
}

function withoutTrailingZeros(text: string): string {
  const trimmed = text.includes('.') ? text.replace(/\.?0+$/, '') : text;
  return trimmed === '-0' ? '0' : trimmed;
}
