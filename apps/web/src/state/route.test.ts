import { describe, expect, it } from 'vitest';

import { comparisonsHref, modelsHref, parseRoute, researchHref, walkForwardHref } from './route';

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

  it('opens walk-forward evaluation, a reviewed plan, or a study inside Research', () => {
    const home = { view: 'walk-forward', planId: null, studyId: null };
    expect(parseRoute(walkForwardHref())).toEqual(home);
    expect(parseRoute(walkForwardHref({ planId: 'd'.repeat(64) }))).toEqual({ ...home, planId: 'd'.repeat(64) });
    expect(parseRoute(walkForwardHref({ studyId: 'e'.repeat(64) }))).toEqual({ ...home, studyId: 'e'.repeat(64) });
    expect(walkForwardHref({ studyId: 'e'.repeat(64) })).toBe(`#/research/walk-forward/studies/${'e'.repeat(64)}`);
    // Anything that is not an ID opens its home, never a path.
    expect(parseRoute('#/research/walk-forward/plans/..%2F..%2Fetc')).toEqual(home);
    expect(parseRoute(`#/research/walk-forward/studies/${'E'.repeat(64)}`)).toEqual(home);
    expect(parseRoute('#/research/walk-forwardx')).toEqual({ view: 'research', resultId: null });
    // The rest of Research is unchanged.
    expect(parseRoute('#/research/results/x')).toEqual({ view: 'research', resultId: null });
  });
});
