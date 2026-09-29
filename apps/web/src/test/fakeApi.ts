// An in-memory stand-in for the C++ API at the fetch boundary. Tests declare
// responses per route and assert on the requests the application sent.
import { createChronosClient } from '../api/client';
import type { ChronosClient } from '../api/client';

export interface RecordedRequest {
  readonly method: string;
  readonly path: string;
  readonly query: URLSearchParams;
  readonly body: unknown;
}

export interface FakeResponse {
  readonly status: number;
  readonly body?: unknown;
  // Replaces the JSON body, for malformed-response tests.
  readonly rawBody?: string;
  readonly omitTradingModeHeader?: boolean;
}

type Responder = (request: RecordedRequest) => FakeResponse | Promise<FakeResponse>;

export interface FakeApi {
  readonly client: ChronosClient;
  readonly requests: RecordedRequest[];
  on(method: 'GET' | 'POST', path: string, respond: Responder | FakeResponse): void;
  // Requests sent to one method and path, in order.
  requestsTo(method: 'GET' | 'POST', path: string): RecordedRequest[];
  // Makes the next matching request fail in transit.
  failNext(method: 'GET' | 'POST', path: string): void;
}

// Rejects with the signal's reason when it aborts, as fetch does.
function abortable<T>(pending: Promise<T>, signal: AbortSignal | undefined): Promise<T> {
  if (!signal) {
    return pending;
  }
  if (signal.aborted) {
    return Promise.reject(signal.reason as Error);
  }
  return new Promise<T>((resolve, reject) => {
    signal.addEventListener('abort', () => {
      reject(signal.reason as Error);
    });
    pending.then(resolve, reject);
  });
}

export function createFakeApi(): FakeApi {
  const requests: RecordedRequest[] = [];
  const routes = new Map<string, Responder>();
  const failures = new Set<string>();

  const fetchImpl: typeof fetch = async (input, init) => {
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
      throw new TypeError('Failed to fetch');
    }
    const respond = routes.get(key);
    if (!respond) {
      throw new Error(`no fake response for ${key}`);
    }
    const response = await abortable(Promise.resolve(respond(request)), init?.signal ?? undefined);
    const headers = new Headers({ 'Content-Type': 'application/json' });
    if (response.omitTradingModeHeader !== true) {
      headers.set('X-Chronos-Trading-Mode', 'paper');
    }
    return new Response(response.rawBody ?? JSON.stringify(response.body), {
      status: response.status,
      headers,
    });
  };

  return {
    client: createChronosClient({ fetch: fetchImpl, readTimeoutMs: 5_000, writeTimeoutMs: 5_000 }),
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
