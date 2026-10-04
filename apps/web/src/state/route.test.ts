import { describe, expect, it } from 'vitest';

import { parseRoute, researchHref } from './route';

describe('parseRoute', () => {
  it('selects research only for its own hash and keeps section links in trading', () => {
    expect(parseRoute('')).toEqual({ view: 'trading' });
    expect(parseRoute('#/')).toEqual({ view: 'trading' });
    expect(parseRoute('#portfolio')).toEqual({ view: 'trading' });
    expect(parseRoute('#/research')).toEqual({ view: 'research', resultId: null });
    expect(parseRoute(researchHref('a'.repeat(64)))).toEqual({ view: 'research', resultId: 'a'.repeat(64) });
    // Anything that is not a result ID opens the research home, never a path.
    expect(parseRoute('#/research/results/..%2F..%2Fetc')).toEqual({ view: 'research', resultId: null });
    expect(parseRoute(`#/research/results/${'A'.repeat(64)}`)).toEqual({ view: 'research', resultId: null });
  });
});
