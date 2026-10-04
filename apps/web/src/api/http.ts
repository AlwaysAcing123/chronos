import type { z } from 'zod';

import { ChronosApiError } from './errors';
import { ErrorEnvelopeSchema } from './schemas';

// The header that proves which Chronos service answered.
export interface ResponseIdentity {
  readonly header: string;
  readonly value: string;
  // For messages, such as "the Chronos paper-trading API".
  readonly service: string;
}

export interface TransportOptions {
  readonly fetch: typeof fetch;
  // Bounds how long a hung server can stall polling or a submission. A
  // submission that times out is treated as uncertain and reconciled.
  readonly readTimeoutMs: number;
  readonly writeTimeoutMs: number;
  // Which service must have answered; the paper-trading API by default.
  readonly identity?: ResponseIdentity;
}

export interface HttpResponse {
  readonly status: number;
  readonly body: unknown;
}

interface RequestSpec {
  readonly method: 'GET' | 'POST';
  readonly path: string;
  readonly body?: unknown;
  readonly signal?: AbortSignal | undefined;
}

// Every Chronos response carries this header; its absence means something
// else answered, such as a development-proxy error page.
export const TRADING_API_IDENTITY: ResponseIdentity = {
  header: 'X-Chronos-Trading-Mode',
  value: 'paper',
  service: 'the Chronos paper-trading API',
};

// Statuses a proxy uses when it could not get an answer from the server.
const GATEWAY_FAILURES: ReadonlySet<number> = new Set([502, 503, 504]);

/**
 * Sends one request and returns its status and parsed JSON body. It throws a
 * ChronosApiError when no Chronos response arrives, and rethrows the
 * caller's abort reason when the caller cancels.
 */
export async function sendRequest(
  transport: TransportOptions,
  { method, path, body, signal }: RequestSpec,
): Promise<HttpResponse> {
  const timeoutMs = method === 'GET' ? transport.readTimeoutMs : transport.writeTimeoutMs;
  const timeout = AbortSignal.timeout(timeoutMs);
  const combined = signal ? AbortSignal.any([signal, timeout]) : timeout;
  // The underlying exception is deliberately not surfaced: it can repeat
  // request details, and the failure kind is what the interface needs.
  const failInTransit = (): never => {
    if (signal?.aborted) {
      throw signal.reason;
    }
    throw new ChronosApiError({
      kind: 'network',
      detail: timeout.aborted
        ? `the Chronos API did not respond within ${String(timeoutMs / 1000)} s`
        : 'the Chronos API could not be reached',
    });
  };

  let response: Response;
  let text: string;
  try {
    response = await transport.fetch(path, {
      method,
      headers:
        body === undefined
          ? { Accept: 'application/json' }
          : { Accept: 'application/json', 'Content-Type': 'application/json' },
      ...(body === undefined ? {} : { body: JSON.stringify(body) }),
      cache: 'no-store',
      signal: combined,
    });
    text = await response.text();
  } catch {
    return failInTransit();
  }

  const identity = transport.identity ?? TRADING_API_IDENTITY;
  if (response.headers.get(identity.header) !== identity.value) {
    if (GATEWAY_FAILURES.has(response.status)) {
      throw new ChronosApiError({ kind: 'network', detail: 'the proxy could not reach the Chronos API' });
    }
    throw new ChronosApiError({
      kind: 'unexpected_response',
      status: response.status,
      detail: `the response did not come from ${identity.service}`,
    });
  }
  try {
    return { status: response.status, body: JSON.parse(text) as unknown };
  } catch {
    throw new ChronosApiError({
      kind: 'unexpected_response',
      status: response.status,
      detail: 'the response body is not JSON',
    });
  }
}

/** Decodes a successful body, or reports the mismatch as an unexpected response. */
export function decodeBody<Schema extends z.ZodType>(
  schema: Schema,
  response: HttpResponse,
): z.infer<Schema> {
  const decoded = schema.safeParse(response.body);
  if (!decoded.success) {
    const issue = decoded.error.issues[0];
    const location = issue && issue.path.length > 0 ? ` at ${issue.path.join('.')}` : '';
    throw new ChronosApiError({
      kind: 'unexpected_response',
      status: response.status,
      detail: `the response does not match the API contract${location}: ${issue?.message ?? 'invalid'}`,
    });
  }
  return decoded.data;
}

/** The error a non-success response describes, or an unexpected-response error. */
export function failureOf(response: HttpResponse): ChronosApiError {
  const envelope = ErrorEnvelopeSchema.safeParse(response.body);
  if (!envelope.success) {
    return new ChronosApiError({
      kind: 'unexpected_response',
      status: response.status,
      detail: 'the error response does not match the API error envelope',
    });
  }
  return new ChronosApiError({
    kind: 'api_error',
    status: response.status,
    error: envelope.data.error,
  });
}
