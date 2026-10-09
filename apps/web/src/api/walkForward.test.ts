import { describe, expect, it } from 'vitest';

import { createFakeResearch } from '../test/fakeResearch';
import type { Json } from '../test/fixtures';
import {
  PARTIAL_STUDY_ID,
  PLAN_ID,
  STUDY_JOB_ID,
  WEEK_COLLECTION_ID,
  collectionPageJson,
  foldResultJson,
  partialStudyDetailJson,
  planJobJson,
  planJson,
  studyJobJson,
  studyJobPageJson,
} from '../test/walkForwardFixtures';
import { DEFAULT_CONFIGURATION } from '../test/researchFixtures';
import { ChronosApiError } from './errors';
import { RESEARCH_API } from './research';
import { ConfigurationSchema } from './researchSchemas';
import type { WalkForwardRequest } from './walkForwardSchemas';

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

function at(document: Json, ...path: (string | number)[]): Json {
  let value: unknown = document;
  for (const key of path) {
    value = (value as Record<string | number, unknown>)[key];
  }
  return value as Json;
}

describe('walk-forward decoders and requests', () => {
  it('decodes the real service’s collections, plan, partial study, jobs, and fold run', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/collections`, { status: 200, body: collectionPageJson() });
    research.on('GET', `${RESEARCH_API}/walk-forward/plans/${PLAN_ID}`, { status: 200, body: planJson() });
    research.on('GET', `${RESEARCH_API}/walk-forward/studies/${PARTIAL_STUDY_ID}`, {
      status: 200,
      body: partialStudyDetailJson(),
    });
    research.on('GET', `${RESEARCH_API}/jobs`, { status: 200, body: studyJobPageJson() });
    research.on('GET', `${RESEARCH_API}/jobs/${STUDY_JOB_ID}`, { status: 200, body: { job: studyJobJson() } });
    const collections = await research.client.listCollections(null);
    expect(collections.collections.map((entry) => entry.members.length)).toEqual(expect.arrayContaining([1, 3]));
    const plan = await research.client.getWalkForwardPlan(PLAN_ID);
    expect(plan.folds.map((fold) => fold.evaluation.window.name)).toEqual(['fold_1', 'fold_2']);
    const study = await research.client.getWalkForwardStudy(PARTIAL_STUDY_ID);
    expect(study.study.status).toBe('partial');
    expect(study.study.folds[0]?.models.logistic.state).toBe('unevaluable');
    const jobs = await research.client.listStudyJobs(null);
    expect(jobs.jobs[0]?.progress).toEqual({ stage: 'publish' });
    expect(research.requestsTo('GET', `${RESEARCH_API}/jobs`)[0]?.query.get('kind')).toBe('walk_forward');
    const job = await research.client.getWalkForwardJob(STUDY_JOB_ID);
    expect(job.kind).toBe('walk_forward');
  });

  it.each([
    ['a completed cell without its deltas', (document: Json) => {
      delete at(document, 'study', 'folds', 1, 'models', 'majority', 'cells', 0).deltas;
    }],
    ['a window that is not a fold', (document: Json) => {
      at(document, 'study', 'folds', 0, 'evaluation', 'window').name = 'test';
    }],
    ['an evidence class that does not exist', (document: Json) => {
      at(document, 'study', 'collection').evidence = 'real';
    }],
    ['a figure as a JSON number', (document: Json) => {
      at(document, 'study', 'aggregates', 'trading', 'majority', 'base', 'complete').starting_capital = 20000;
    }],
    ['an unknown cell state', (document: Json) => {
      at(document, 'study', 'folds', 0, 'models', 'logistic', 'cells', 0).state = 'skipped';
    }],
  ])('refuses %s as an unexpected response', async (_, edit) => {
    const document = partialStudyDetailJson();
    edit(document);
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/walk-forward/studies/${PARTIAL_STUDY_ID}`, { status: 200, body: document });
    const failure = await failureOf(research.client.getWalkForwardStudy(PARTIAL_STUDY_ID));
    expect(failure.failure.kind).toBe('unexpected_response');
  });

  it('refuses a progress report it does not know', async () => {
    const research = createFakeResearch();
    research.on('GET', `${RESEARCH_API}/jobs/${STUDY_JOB_ID}`, {
      status: 200,
      body: { job: studyJobJson({ progress: { stage: 'resuming' } }) },
    });
    expect((await failureOf(research.client.getWalkForwardJob(STUDY_JOB_ID))).failure.kind).toBe('unexpected_response');
  });

  it('sends Preview and Start to their routes, Start naming only the plan', async () => {
    const research = createFakeResearch();
    research.on('POST', `${RESEARCH_API}/walk-forward/plans`, { status: 202, body: { deduplicated: false, job: planJobJson() } });
    research.on('POST', `${RESEARCH_API}/walk-forward/studies`, {
      status: 200,
      body: { deduplicated: true, job: studyJobJson() },
    });
    const request: WalkForwardRequest = {
      collection_id: WEEK_COLLECTION_ID,
      instrument: 'AAPL',
      minimum_training_sources: '1',
      configuration: ConfigurationSchema.parse(DEFAULT_CONFIGURATION),
      scenarios: [{ name: 'base', fee_per_fill: '0.01', latency_ns: '1000000' }],
    };
    const preview = await research.client.previewWalkForward(request);
    expect(preview.job.result?.plan_id).toBe(PLAN_ID);
    const started = await research.client.startWalkForwardStudy(PLAN_ID);
    expect(started.deduplicated).toBe(true);
    expect(research.requestsTo('POST', `${RESEARCH_API}/walk-forward/plans`)[0]?.body).toEqual(request);
    expect(research.requestsTo('POST', `${RESEARCH_API}/walk-forward/studies`)[0]?.body).toEqual({ plan_id: PLAN_ID });
  });

  it('decodes a fold run’s result with its fold window and provenance', async () => {
    const research = createFakeResearch();
    const document = foldResultJson();
    const runId = String(at(document, 'result').run_id);
    research.on('GET', `${RESEARCH_API}/results/${runId}`, { status: 200, body: document });
    const result = await research.client.getResult(runId);
    expect(result.window.name).toBe('fold_2');
    expect('fold_model_id' in result.source && result.source.plan_id).toBe(PLAN_ID);
  });
});
