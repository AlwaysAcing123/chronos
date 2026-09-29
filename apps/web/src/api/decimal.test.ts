import { describe, expect, it } from 'vitest';

import { decimalSign, isCanonicalDecimal, sameDecimal, validatePositiveDecimal } from './decimal';

describe('validatePositiveDecimal', () => {
  it('returns accepted text unchanged, including trailing zeros', () => {
    expect(validatePositiveDecimal('10.50', 'quantity')).toEqual({ ok: true, value: '10.50' });
    expect(validatePositiveDecimal('0.00000001', 'quantity')).toEqual({ ok: true, value: '0.00000001' });
    expect(validatePositiveDecimal('337.000000001', 'price')).toEqual({ ok: true, value: '337.000000001' });
  });

  it('rejects text the server grammar rejects', () => {
    for (const text of ['1e3', '+1', '01', '1.', '.5', ' 1', '1 ', '1,000', '١', '0x1', 'NaN']) {
      expect(validatePositiveDecimal(text, 'quantity'), text).toEqual({ ok: false, problem: 'invalid_format' });
    }
    expect(validatePositiveDecimal('', 'money')).toEqual({ ok: false, problem: 'required' });
  });

  it('requires a positive value', () => {
    for (const text of ['0', '0.0', '-1', '-0.5']) {
      expect(validatePositiveDecimal(text, 'quantity'), text).toEqual({ ok: false, problem: 'not_positive' });
    }
  });

  it('enforces each fixed-point scale instead of rounding', () => {
    expect(validatePositiveDecimal('1.123456789', 'quantity')).toEqual({
      ok: false,
      problem: 'too_many_fractional_digits',
    });
    expect(validatePositiveDecimal('1.123456789', 'price')).toEqual({ ok: true, value: '1.123456789' });
    expect(validatePositiveDecimal('1.1234567891', 'price')).toEqual({
      ok: false,
      problem: 'too_many_fractional_digits',
    });
  });

  it('accepts the largest representable value and rejects one unit more', () => {
    expect(validatePositiveDecimal('92233720368.54775807', 'money').ok).toBe(true);
    expect(validatePositiveDecimal('92233720368.54775808', 'money')).toEqual({ ok: false, problem: 'out_of_range' });
    expect(validatePositiveDecimal('9223372036.854775807', 'price').ok).toBe(true);
    expect(validatePositiveDecimal('9223372036.854775808', 'price')).toEqual({ ok: false, problem: 'out_of_range' });
    expect(validatePositiveDecimal('100000000000', 'quantity')).toEqual({ ok: false, problem: 'out_of_range' });
  });
});

describe('canonical response decimals', () => {
  it('accepts the server rendering and nothing else', () => {
    for (const text of ['0', '101', '0.01', '-2.5', '-0.00542', '99155.03']) {
      expect(isCanonicalDecimal(text, 8), text).toBe(true);
    }
    for (const text of ['-0', '1.50', '1.', '01', '+1', '1e2', '']) {
      expect(isCanonicalDecimal(text, 8), text).toBe(false);
    }
    expect(isCanonicalDecimal('0.123456789', 8)).toBe(false);
    expect(isCanonicalDecimal('0.123456789', 9)).toBe(true);
  });

  it('reads the sign from the text', () => {
    expect(decimalSign('-5.42')).toBe('negative');
    expect(decimalSign('0')).toBe('zero');
    expect(decimalSign('0.00000001')).toBe('positive');
  });

  it('compares values without converting them to numbers', () => {
    expect(sameDecimal('10.50', '10.5')).toBe(true);
    expect(sameDecimal('10.00', '10')).toBe(true);
    expect(sameDecimal('100', '10')).toBe(false);
    // A float comparison would call these equal.
    expect(sameDecimal('9007199254740993', '9007199254740992')).toBe(false);
  });
});
