// The demo guide's one read, from the research service (ADR 0025). Only a
// demo build creates this client, so an ordinary build never asks for it.
import { DemoGuideSchema } from './demoSchemas';
import type { DemoGuide } from './demoSchemas';
import type { TransportOptions } from './http';
import { decodeBody, failureOf, sendRequest } from './http';
import { RESEARCH_API, RESEARCH_IDENTITY } from './research';

export interface DemoClient {
  getGuide(signal?: AbortSignal): Promise<DemoGuide>;
}

const DEFAULT_DEMO_TRANSPORT: TransportOptions = {
  fetch: (input, init) => fetch(input, init),
  readTimeoutMs: 10_000,
  writeTimeoutMs: 10_000,
  identity: RESEARCH_IDENTITY,
};

export function createDemoClient(transport: TransportOptions = DEFAULT_DEMO_TRANSPORT): DemoClient {
  const options = { ...transport, identity: transport.identity ?? RESEARCH_IDENTITY };
  return {
    async getGuide(signal) {
      const response = await sendRequest(options, { method: 'GET', path: `${RESEARCH_API}/demo`, signal });
      if (response.status !== 200) {
        throw failureOf(response);
      }
      return decodeBody(DemoGuideSchema, response);
    },
  };
}
