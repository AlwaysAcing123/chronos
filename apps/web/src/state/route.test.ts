import { describe, expect, it } from 'vitest';

import { comparisonsHref, modelsHref, parseRoute, researchHref } from './route';

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

  it('opens the model library or one version, and nothing else, under #/models', () => {
    expect(parseRoute('#/models')).toEqual({ view: 'models', modelId: null });
    expect(parseRoute(modelsHref('b'.repeat(64)))).toEqual({ view: 'models', modelId: 'b'.repeat(64) });
    expect(parseRoute('#/models/..%2Fexperiments')).toEqual({ view: 'models', modelId: null });
    expect(parseRoute(`#/models/${'b'.repeat(63)}`)).toEqual({ view: 'models', modelId: null });
    expect(parseRoute('#/modelsx')).toEqual({ view: 'trading' });
  });

  it('opens the comparisons or one comparison under #/comparisons', () => {
    expect(parseRoute('#/comparisons')).toEqual({ view: 'comparisons', comparisonId: null });
    expect(parseRoute(comparisonsHref('c'.repeat(64)))).toEqual({ view: 'comparisons', comparisonId: 'c'.repeat(64) });
    expect(parseRoute('#/comparisons/..%2Fmodels')).toEqual({ view: 'comparisons', comparisonId: null });
  });
});
