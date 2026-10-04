// A small stateful stand-in for the research service, answered inside the
// browser like mockApi.ts answers for the trading API. A started job is
// queued, then running, then finished on later polls.
import type { Page, Route } from '@playwright/test';

import { errorJson } from '../src/test/fixtures';
import type { Json } from '../src/test/fixtures';
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
