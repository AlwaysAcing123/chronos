import { describe, expect, it } from 'vitest';

import { timestampNanoseconds } from '../components/research/EquityChart';
import { ppbAsPercent } from './research';

describe('research formatting', () => {
  it('writes parts per billion as an exact percentage', () => {
    expect(ppbAsPercent('500000000')).toBe('50');
    expect(ppbAsPercent('501539269')).toBe('50.1539269');
    expect(ppbAsPercent('1000000000')).toBe('100');
    expect(ppbAsPercent('5')).toBe('0.0000005');
    expect(ppbAsPercent('0')).toBe('0');
  });

  it('reads a timestamp to the nanosecond for chart geometry', () => {
    expect(timestampNanoseconds('2026-09-25T13:39:59.969574414Z')).toBe(1_790_343_599_969_574_414n);
    expect(timestampNanoseconds('1970-01-01T00:00:00.000000001Z')).toBe(1n);
  });
});
