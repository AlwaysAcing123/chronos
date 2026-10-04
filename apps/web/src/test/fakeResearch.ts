// An in-memory stand-in for the research service at the fetch boundary, like
// fakeApi.ts for the trading API: tests declare responses per route and
// assert on the requests the page sent.
import type { ResearchClient } from '../api/research';
import { createResearchClient } from '../api/research';
import type { RecordedRequest } from './fakeApi';

export interface FakeResearchResponse {
  readonly status: number;
  readonly body?: unknown;
  // Leaves out the service header, as a proxy error page would.
  readonly omitServiceHeader?: boolean;
}

type Responder = (request: RecordedRequest) => FakeResearchResponse;

export interface FakeResearch {
  readonly client: ResearchClient;
  readonly requests: RecordedRequest[];
  on(method: 'GET' | 'POST', path: string, respond: Responder | FakeResearchResponse): void;
  requestsTo(method: 'GET' | 'POST', path: string): RecordedRequest[];
  failNext(method: 'GET' | 'POST', path: string): void;
}

export function createFakeResearch(): FakeResearch {
  const requests: RecordedRequest[] = [];
  const routes = new Map<string, Responder>();
  const failures = new Set<string>();
  const fetchImpl: typeof fetch = (input, init) => {
    const target = typeof input === 'string' ? input : input instanceof URL ? input.href : input.url;
    const url = new URL(target, 'http://chronos.test');
    const method = init?.method ?? 'GET';
    const bodyText = typeof init?.body === 'string' ? init.body : undefined;
    const request: RecordedRequest = {
      method,
      path: url.pathname,
      query: url.searchParams,
      body: bodyText === undefined ? undefined : (JSON.parse(bodyText) as unknown),
    };
    requests.push(request);
    const key = `${method} ${url.pathname}`;
    if (failures.delete(key)) {
      return Promise.reject(new TypeError('Failed to fetch'));
    }
    const respond = routes.get(key);
    if (!respond) {
      return Promise.reject(new Error(`no fake research response for ${key}`));
    }
    const response = respond(request);
    const headers = new Headers({ 'Content-Type': 'application/json' });
    if (response.omitServiceHeader !== true) {
      headers.set('X-Chronos-Service', 'research');
    }
    return Promise.resolve(new Response(JSON.stringify(response.body ?? {}), { status: response.status, headers }));
  };
  return {
    client: createResearchClient({ fetch: fetchImpl, readTimeoutMs: 5_000, writeTimeoutMs: 5_000 }),
    requests,
    on(method, path, respond) {
      routes.set(`${method} ${path}`, typeof respond === 'function' ? respond : () => respond);
    },
    requestsTo(method, path) {
      return requests.filter((request) => request.method === method && request.path === path);
    },
    failNext(method, path) {
      failures.add(`${method} ${path}`);
    },
  };
}
