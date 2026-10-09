// A small stateful stand-in for the research service, answered inside the
// browser like mockApi.ts answers for the trading API. A started job is
// queued, then running, then finished on later polls.
import type { Page, Route } from '@playwright/test';

import {
  COMPARISON_ID,
  COMPARISON_JOB_ID,
  comparisonDetailJson,
  comparisonJobJson,
  comparisonPageJson,
} from '../src/test/comparisonFixtures';
import { errorJson } from '../src/test/fixtures';
import {
  ONE_RECORDING_COLLECTION_ID,
  PARTIAL_STUDY_ID,
  PLAN_ID,
  PLAN_JOB_ID,
  STUDY_JOB_ID,
  WEEK_COLLECTION_ID,
  collectionJson,
  collectionPageJson,
  partialStudyDetailJson,
  planJobJson,
  planJson,
  studyJobJson,
  studyPageJson,
} from '../src/test/walkForwardFixtures';
import type { Json } from '../src/test/fixtures';
import {
  IMPORTED_VERSION_ID,
  IMPORT_ID,
  IMPORT_JOB_ID,
  importJobJson,
  importedDetailJson,
  importedListingJson,
  stagedImportJson,
} from '../src/test/importFixtures';
import {
  MODEL_VERSION_ID,
  REGISTRATION_JOB_ID,
  modelDetailJson,
  modelListingJson,
  registrationJobJson,
} from '../src/test/modelFixtures';
import {
  RUN_ID,
  decisionsJson,
  equityPointsJson,
  experimentPageJson,
  jobJson,
  researchStatusJson,
  resultJson,
  resultListingJson,
  succeededJobJson,
  tradesJson,
} from '../src/test/researchFixtures';

export interface ResearchMockOptions {
  // Every request fails as a proxy does when nothing listens.
  readonly unreachable?: boolean;
  readonly emptyCatalog?: boolean;
  // A started job fails instead of succeeding.
  readonly failJob?: boolean;
  // The result's journal does not match its manifest.
  readonly corruptResult?: boolean;
  // The library already holds a registered and an imported AAPL version.
  readonly seededLibrary?: boolean;
  // The only collection holds one recording, too few for a walk-forward study.
  readonly oneRecording?: boolean;
  // The study's fold 2 logistic base run journal no longer matches its manifest.
  readonly corruptStudyRun?: boolean;
}

function reply(route: Route, status: number, body: unknown) {
  return route.fulfill({
    status,
    contentType: 'application/json',
    headers: { 'X-Chronos-Service': 'research', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' },
    body: JSON.stringify(body),
  });
}

export async function mockResearchService(page: Page, options: ResearchMockOptions = {}) {
  let job: Json | null = null;
  let polls = 0;
  const published = () => job !== null && job.state === 'succeeded';
  // The model library: a registration is queued, then running, then done.
  let registration: Json | null = null;
  let registrationPolls = 0;
  let lifecycle: Json = { state: 'active', reason: null, changed_at: null };
  const registered = () => registration !== null && registration.state === 'succeeded';
  // An import: staged, its bytes uploaded and hashed here, then validated.
  let uploaded: Json | null = null;
  let importJob: Json | null = null;
  let importPolls = 0;
  const imported = () => importJob !== null && importJob.state === 'succeeded';
  // A comparison: queued, running, then published.
  let comparisonJob: Json | null = null;
  let comparisonPolls = 0;
  // Walk-forward: a Preview job, then a study job reporting its folds.
  let planJob: Json | null = null;
  let planPolls = 0;
  let studyJob: Json | null = null;
  let studyPolls = 0;

  await page.route('**/api/v1/research/**', async (route) => {
    if (options.unreachable === true) {
      return route.fulfill({ status: 502, contentType: 'text/plain', body: 'Bad Gateway' });
    }
    const request = route.request();
    const url = new URL(request.url());
    const path = url.pathname.replace('/api/v1/research', '');
    if (path === '/status') {
      return reply(route, 200, researchStatusJson());
    }
    if (path === '/experiments') {
      return reply(route, 200, options.emptyCatalog === true ? experimentPageJson([]) : experimentPageJson());
    }
    if (path === '/runs' && request.method() === 'POST') {
      job = jobJson({ request: request.postDataJSON() as Json });
      polls = 0;
      return reply(route, 202, { deduplicated: false, job });
    }
    if (path === '/runs') {
      if (job !== null) {
        polls += 1;
        if (polls === 1) {
          job = { ...job, state: 'running', started_at: '2026-10-04T05:40:00.100000000Z' };
        } else if (polls >= 2 && job.state === 'running') {
          job =
            options.failJob === true
              ? {
                  ...job,
                  state: 'failed',
                  finished_at: '2026-10-04T05:40:02.000000000Z',
                  error: { code: 'evaluation_failed', message: 'the split artifact was refused' },
                }
              : succeededJobJson({ request: job.request });
        }
      }
      return reply(route, 200, { runs: job === null ? [] : [job], next_cursor: null });
    }
    if (path === '/models' && request.method() === 'POST') {
      registration = registrationJobJson({ request: request.postDataJSON() as Json });
      registrationPolls = 0;
      return reply(route, 202, { deduplicated: false, job: registration });
    }
    if (path === '/models') {
      const state = url.searchParams.get('state') ?? 'active';
      const listed = (registered() || options.seededLibrary === true) && (state === 'all' || state === lifecycle.state);
      return reply(route, 200, {
        models: [
          ...(listed ? [modelListingJson({ lifecycle })] : []),
          ...((imported() || options.seededLibrary === true) && state !== 'archived' ? [importedListingJson()] : []),
        ],
        invalid_entries: [],
        next_cursor: null,
      });
    }
    if (path === '/comparisons' && request.method() === 'POST') {
      comparisonJob = comparisonJobJson({
        request: request.postDataJSON() as Json,
        state: 'queued',
        result: null,
        started_at: null,
        finished_at: null,
      });
      comparisonPolls = 0;
      return reply(route, 202, { deduplicated: false, job: comparisonJob });
    }
    if (path === '/comparisons') {
      const page = comparisonPageJson();
      return reply(route, 200, comparisonJob?.state === 'succeeded' ? page : { ...page, comparisons: [] });
    }
    if (path === `/jobs/${COMPARISON_JOB_ID}` && comparisonJob !== null) {
      comparisonPolls += 1;
      comparisonJob =
        comparisonPolls === 1
          ? { ...comparisonJob, state: 'running', started_at: '2026-10-07T22:00:01.000000000Z' }
          : comparisonJobJson({ request: comparisonJob.request });
      return reply(route, 200, { job: comparisonJob });
    }
    if (path === `/comparisons/${COMPARISON_ID}`) {
      return reply(route, 200, comparisonDetailJson());
    }
    if (path === '/imports' && request.method() === 'POST') {
      uploaded = null;
      return reply(route, 201, stagedImportJson());
    }
    if (path === `/imports/${IMPORT_ID}/model` && request.method() === 'PUT') {
      const bytes = request.postDataBuffer() ?? Buffer.alloc(0);
      const digest = Buffer.from(await crypto.subtle.digest('SHA-256', new Uint8Array(bytes))).toString('hex');
      uploaded = { sha256: digest, size_bytes: String(bytes.length), uploaded_at: '2026-10-07T21:00:01.000000000Z' };
      return reply(route, 201, stagedImportJson(uploaded));
    }
    if (path === `/imports/${IMPORT_ID}/validation` && request.method() === 'POST' && uploaded !== null) {
      const body = request.postDataJSON() as Json;
      if (body.model_sha256 !== uploaded.sha256) {
        return reply(route, 409, errorJson('model_mismatch', 'the staged model is not the one reviewed'));
      }
      importJob = importJobJson({ request: { ...(importJobJson().request as Json), model_sha256: uploaded.sha256 } });
      importPolls = 0;
      return reply(route, 202, { deduplicated: false, job: importJob });
    }
    if (path === `/jobs/${IMPORT_JOB_ID}` && importJob !== null) {
      importPolls += 1;
      importJob =
        importPolls === 1
          ? { ...importJob, state: 'running', started_at: '2026-10-07T21:00:03.000000000Z' }
          : {
              ...importJob,
              state: 'succeeded',
              finished_at: '2026-10-07T21:00:05.000000000Z',
              result: {
                model_version_id: IMPORTED_VERSION_ID,
                outcome: 'published',
                binding_outcome: 'published',
                binding_id: 'b'.repeat(64),
              },
            };
      return reply(route, 200, { job: importJob });
    }
    if (path === `/models/${IMPORTED_VERSION_ID}`) {
      return reply(route, 200, { model: importedDetailJson() });
    }
    if (path === `/jobs/${REGISTRATION_JOB_ID}` && registration !== null) {
      registrationPolls += 1;
      registration =
        registrationPolls === 1
          ? { ...registration, state: 'running', started_at: '2026-10-07T20:00:01.000000000Z' }
          : {
              ...registration,
              state: 'succeeded',
              finished_at: '2026-10-07T20:00:04.000000000Z',
              result: {
                model_version_id: MODEL_VERSION_ID,
                outcome: 'published',
                binding_outcome: 'published',
                binding_id: 'b'.repeat(64),
              },
            };
      return reply(route, 200, { job: registration });
    }
    if (path === `/models/${MODEL_VERSION_ID}`) {
      return reply(route, 200, { model: modelDetailJson({ lifecycle }) });
    }
    if (path === `/models/${MODEL_VERSION_ID}/lifecycle` && request.method() === 'POST') {
      const body = request.postDataJSON() as Json;
      lifecycle = { state: body.state, reason: body.reason, changed_at: '2026-10-07T20:01:00.000000000Z' };
      return reply(route, 200, { model_version_id: MODEL_VERSION_ID, lifecycle });
    }
    if (path === '/collections') {
      return reply(
        route,
        200,
        collectionPageJson(
          options.oneRecording === true
            ? [collectionJson(ONE_RECORDING_COLLECTION_ID)]
            : [collectionJson(WEEK_COLLECTION_ID), collectionJson(ONE_RECORDING_COLLECTION_ID)],
        ),
      );
    }
    if (path === '/walk-forward/plans' && request.method() === 'POST') {
      planJob = planJobJson({ request: request.postDataJSON() as Json, state: 'queued', result: null, finished_at: null });
      planPolls = 0;
      return reply(route, 202, { deduplicated: false, job: planJob });
    }
    if (path === `/jobs/${PLAN_JOB_ID}` && planJob !== null) {
      planPolls += 1;
      planJob = planPolls === 1 ? { ...planJob, state: 'running' } : planJobJson({ request: planJob.request });
      return reply(route, 200, { job: planJob });
    }
    if (path === `/walk-forward/plans/${PLAN_ID}`) {
      return reply(route, 200, planJson());
    }
    if (path === '/walk-forward/studies' && request.method() === 'POST') {
      studyJob = studyJobJson({ state: 'queued', progress: null, result: null, finished_at: null });
      studyPolls = 0;
      return reply(route, 202, { deduplicated: false, job: studyJob });
    }
    if (path === `/jobs/${STUDY_JOB_ID}` && studyJob !== null) {
      studyPolls += 1;
      const progress = [
        { stage: 'verify_sources' },
        { fold: '1', folds: '2', stage: 'fold', step: 'fit' },
        { fold: '2', folds: '2', model: 'logistic', scenario: 'stress', stage: 'fold', step: 'run' },
      ][studyPolls - 1];
      studyJob =
        progress === undefined
          ? studyJobJson({
              result: { missing: '1', outcome: 'published', status: 'partial', study_id: PARTIAL_STUDY_ID },
            })
          : { ...studyJob, state: 'running', progress };
      return reply(route, 200, { job: studyJob });
    }
    if (path === '/jobs' && url.searchParams.get('kind') === 'walk_forward') {
      return reply(route, 200, { jobs: studyJob === null ? [] : [studyJob], next_cursor: null });
    }
    if (path === '/walk-forward/studies') {
      const page = studyPageJson();
      return reply(route, 200, studyJob?.state === 'succeeded' ? page : { ...page, studies: [] });
    }
    if (path === `/walk-forward/studies/${PARTIAL_STUDY_ID}`) {
      const detail = partialStudyDetailJson();
      if (options.corruptStudyRun === true) {
        const [, second] = detail.artifacts as Json[];
        const runs = ((second?.models as Record<string, Json>).logistic as { runs: Record<string, string> }).runs;
        runs.base = 'invalid: journal.ndjson differs from its manifest’s checksum';
      }
      return reply(route, 200, detail);
    }
    if (path === '/results') {
      return reply(route, 200, { results: published() ? [resultListingJson()] : [], invalid_entries: [], next_cursor: null });
    }
    if (path === `/results/${RUN_ID}`) {
      return options.corruptResult === true
        ? reply(route, 422, errorJson('artifact_invalid', 'the result cannot be read: journal.ndjson differs from its manifest’s checksum'))
        : reply(route, 200, { result: resultJson() });
    }
    if (path === `/results/${RUN_ID}/equity`) {
      return reply(route, 200, { points: equityPointsJson(), next_cursor: null });
    }
    if (path === `/results/${RUN_ID}/trades`) {
      const trades = tradesJson();
      return url.searchParams.get('cursor') === null
        ? reply(route, 200, { trades: trades.slice(0, 2), next_cursor: 'djF8dHJhZGVzfDI' })
        : reply(route, 200, { trades: trades.slice(2), next_cursor: null });
    }
    if (path === `/results/${RUN_ID}/decisions`) {
      return reply(route, 200, { decisions: decisionsJson(), next_cursor: null });
    }
    return reply(route, 404, errorJson('route_not_found', 'no such path'));
  });
}
