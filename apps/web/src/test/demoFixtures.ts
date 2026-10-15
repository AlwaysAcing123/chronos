// The demo guide's data as a demo workspace's research service returns it
// (GET /api/v1/research/demo, ADR 0025): IDs, routes, and verdicts only.
import type { Json } from './fixtures';

const id = (digit: string) => digit.repeat(64);

export const DEMO_IDS = {
  seed: id('5'),
  playback: id('4'),
  logistic: id('a'),
  majority: id('b'),
  comparison: id('c'),
  studyAapl: id('d'),
  studySpy: id('e'),
  diagnostics: id('f'),
  run: id('1'),
};

function session(name: string, role: string, day: string, minutes: number, dataset: string): Json {
  return {
    name,
    role,
    day,
    declared_label: `synthetic ${name} (${day}), invented prices`,
    stream: `chronos-demo-v1-${name}`,
    dataset_id: dataset,
    events: role === 'playback' ? '24053' : '6013',
    feature_id: role === 'playback' ? null : id('7'),
    label_id: role === 'playback' ? null : id('8'),
    receive_first: `${day}T14:30:00.025874756Z`,
    receive_last: `${day}T14:${(30 + minutes - 1).toString()}:59.912000000Z`,
    table_sha256: id('9'),
  };
}

export function demoGuideJson({
  mode = 'playback',
  number = '1',
  diagnosticsRun = `#/research/walk-forward/diagnostics/${DEMO_IDS.diagnostics}/runs/${DEMO_IDS.run}/strategy`,
}: { mode?: 'playback' | 'history'; number?: string; diagnosticsRun?: string | Json } = {}): Json {
  return {
    session: { mode, number },
    seed: {
      schema: { name: 'chronos.demo_seed', version: '1' },
      seed_id: DEMO_IDS.seed,
      notice:
        'Every price, size, and time in this workspace was invented by scenario chronos.demo_scenario version 1. AAPL and SPY are used as names only.',
      scenario: {
        name: 'chronos.demo_scenario',
        version: '1',
        evidence: 'synthetic',
        instruments: ['AAPL', 'SPY'],
        sessions: [
          session('h1', 'history', '2026-03-02', 10, id('2')),
          session('h2', 'history', '2026-03-03', 10, id('3')),
          session('h3', 'history', '2026-03-04', 10, id('6')),
          session('playback', 'playback', '2026-03-05', 40, DEMO_IDS.playback),
        ],
      },
      playback: {
        dataset_id: DEMO_IDS.playback,
        stream: 'chronos-demo-v1-playback',
        events: '24053',
        instruments: ['AAPL', 'SPY'],
        receive_first: '2026-03-05T14:30:00.025874756Z',
        receive_last: '2026-03-05T15:09:59.912000000Z',
      },
      chronology: {
        checks: ['every historical session ends before the playback session begins'],
        history_ends: '2026-03-04T14:39:59.912000000Z',
        playback_starts: '2026-03-05T14:30:00.025874756Z',
        model_cutoffs: [
          { family: 'logistic', at: '2026-03-04T14:36:00.100000000Z', model_version_id: DEMO_IDS.logistic },
          { family: 'majority', at: '2026-03-04T14:36:00.100000000Z', model_version_id: DEMO_IDS.majority },
        ],
      },
      audits: {
        walk_forward: { AAPL: { checks_total: '23053', verdict: 'pass' }, SPY: { checks_total: '23911', verdict: 'pass' } },
        strategy_diagnostics: { checks_total: '118654', verdict: 'pass' },
      },
      artifacts: {
        model_versions: { logistic: DEMO_IDS.logistic, majority: DEMO_IDS.majority },
        comparison_id: DEMO_IDS.comparison,
        walk_forward_studies: { AAPL: DEMO_IDS.studyAapl, SPY: DEMO_IDS.studySpy },
        strategy_diagnostics: { diagnostic_id: DEMO_IDS.diagnostics },
      },
      routes: {
        trading: '#/',
        research: '#/research',
        walk_forward: '#/research/walk-forward',
        models: { logistic: `#/models/${DEMO_IDS.logistic}`, majority: `#/models/${DEMO_IDS.majority}` },
        comparison: `#/comparisons/${DEMO_IDS.comparison}`,
        studies: {
          AAPL: `#/research/walk-forward/studies/${DEMO_IDS.studyAapl}`,
          SPY: `#/research/walk-forward/studies/${DEMO_IDS.studySpy}`,
        },
        diagnostics: `#/research/walk-forward/diagnostics/${DEMO_IDS.diagnostics}`,
        diagnostics_run: diagnosticsRun,
      },
    },
  };
}
