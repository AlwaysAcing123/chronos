import { describe, expect, it } from 'vitest';

import { errorJson } from '../test/fixtures';
import { createFakeResearch } from '../test/fakeResearch';
import {
  EXPERIMENT_ID,
  RUN_ID,
  equityPointsJson,
  experimentJson,
  experimentPageJson,
  jobJson,
  resultJson,
  runRequestJson,
  succeededJobJson,
  tradesJson,
} from '../test/researchFixtures';
import { ChronosApiError } from './errors';
import { RESEARCH_API } from './research';
import type { RunRequest } from './researchSchemas';
import { isComplete } from './researchSchemas';

async function failureOf(promise: Promise<unknown>): Promise<ChronosApiError> {
  try {
    await promise;
  } catch (error) {
    if (error instanceof ChronosApiError) {
      return error;
    }
    throw error;
  }
  throw new Error('expected the request to fail');
}

describe('ResearchClient', () => {
  it('decodes the experiment catalog and one experiment', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/experiments`, { status: 200, body: experimentPageJson() });
    research.on('GET', `${RESEARCH_API}/experiments/${EXPERIMENT_ID}`, { status: 200, body: { experiment: experimentJson() } });
    const page = await research.client.listExperiments(null);
    expect(page.experiments[0]?.instruments.map((entry) => entry.models.logistic.status)).toEqual(['unevaluable', 'fitted']);
    expect(research.requestsTo('GET', `${RESEARCH_API}/experiments`)[0]?.query.get('limit')).toBe('50');
    const experiment = await research.client.getExperiment(EXPERIMENT_ID);
    expect(experiment.windows.test.end_at).toBeNull();
  });

  it('trusts only responses that carry the research service header', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/status`, { status: 200, body: {}, omitServiceHeader: true });
    expect((await failureOf(research.client.getStatus())).failure).toMatchObject({ kind: 'unexpected_response' });
    research.on('GET', `${RESEARCH_API}/status`, { status: 502, body: {}, omitServiceHeader: true });
    expect((await failureOf(research.client.getStatus())).failure.kind).toBe('network');
    research.failNext('GET', `${RESEARCH_API}/status`);
    expect((await failureOf(research.client.getStatus())).failure.kind).toBe('network');
  });

  it('starts a run, recognizes a deduplicated one, and reports a refusal with its field', async () => {
    const research = createFakeResearch();
    const request = runRequestJson() as unknown as RunRequest;
    research.on('POST', `${RESEARCH_API}/runs`, { status: 202, body: { deduplicated: false, job: jobJson() } });
    const started = await research.client.startRun(request);
    expect(started.deduplicated).toBe(false);
    expect(research.requestsTo('POST', `${RESEARCH_API}/runs`)[0]?.body).toEqual(request);
    research.on('POST', `${RESEARCH_API}/runs`, { status: 200, body: { deduplicated: true, job: succeededJobJson() } });
    expect((await research.client.startRun(request)).job.result?.run_id).toBe(RUN_ID);
    research.on('POST', `${RESEARCH_API}/runs`, {
      status: 422,
      body: errorJson('invalid_configuration', 'configuration.fee_per_fill must be nonnegative', {
        field: 'configuration.fee_per_fill',
        reason: 'out_of_domain',
      }),
    });
    const refused = await failureOf(research.client.startRun(request));
    expect(refused.failure).toMatchObject({ kind: 'api_error', error: { field: 'configuration.fee_per_fill' } });
  });

  it('reads a whole equity curve page by page and keeps unavailable valuations as such', async () => {
    const research = createFakeResearch();
    const [first, second, third, fourth] = equityPointsJson();
    research.on('GET', `${RESEARCH_API}/results/${RUN_ID}/equity`, (request) =>
      request.query.get('cursor') === null
        ? { status: 200, body: { points: [first, second], next_cursor: 'djF8ZXF1aXR5fDI' } }
        : { status: 200, body: { points: [third, fourth], next_cursor: null } },
    );
    const points = await research.client.getEquity(RUN_ID);
    expect(points).toHaveLength(4);
    expect(research.requestsTo('GET', `${RESEARCH_API}/results/${RUN_ID}/equity`).map((request) => request.query.get('cursor'))).toEqual([
      null,
      'djF8ZXF1aXR5fDI',
    ]);
    const last = points[3];
    expect(last !== undefined && isComplete(last.accounts.buy_and_hold)).toBe(false);
  });

  it('decodes a result and refuses one whose money is not canonical', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/results/${RUN_ID}`, { status: 200, body: { result: resultJson() } });
    const result = await research.client.getResult(RUN_ID);
    expect(result.accounts.strategy.turnover.notional).toBe('200.35');
    const broken = resultJson();
    (broken.configuration as { starting_cash: string }).starting_cash = '10000.00';
    research.on('GET', `${RESEARCH_API}/results/${RUN_ID}`, { status: 200, body: { result: broken } });
    expect((await failureOf(research.client.getResult(RUN_ID))).failure.kind).toBe('unexpected_response');
  });

  it('pages trades by account with the cursor it was given', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/results/${RUN_ID}/trades`, { status: 200, body: { trades: tradesJson(), next_cursor: null } });
    const page = await research.client.listTrades(RUN_ID, 'buy_and_hold', 'abc');
    expect(page.trades.map((trade) => trade.outcome)).toEqual(['filled', 'filled', 'expired']);
    const query = research.requestsTo('GET', `${RESEARCH_API}/results/${RUN_ID}/trades`)[0]?.query;
    expect([query?.get('account'), query?.get('cursor'), query?.get('limit')]).toEqual(['buy_and_hold', 'abc', '50']);
  });
});
