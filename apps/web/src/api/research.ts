// The one boundary between the application and the local research service.
// Like the trading client, every response is decoded before it reaches
// application state, and a response without the service's header is not
// trusted as one.
import type { z } from 'zod';

import { ChronosApiError } from './errors';
import type { HttpResponse, ResponseIdentity, TransportOptions } from './http';
import { decodeBody, failureOf, sendRequest } from './http';
import type {
  AccountName,
  DecisionPage,
  EquityPoint,
  Experiment,
  ExperimentPage,
  Job,
  JobPage,
  ResearchResult,
  ResearchStatus,
  ResultPage,
  RunRequest,
  TradePage,
} from './researchSchemas';
import {
  DecisionPageSchema,
  EquityPageSchema,
  ExperimentEnvelopeSchema,
  ExperimentPageSchema,
  JobEnvelopeSchema,
  JobPageSchema,
  ResultEnvelopeSchema,
  ResultPageSchema,
  StartedJobSchema,
  StatusSchema,
  TradePageSchema,
} from './researchSchemas';

export const RESEARCH_API = '/api/v1/research';

export const RESEARCH_IDENTITY: ResponseIdentity = {
  header: 'X-Chronos-Service',
  value: 'research',
  service: 'the Chronos research service',
};

export const RESEARCH_PAGE_SIZE = 50;
// Equity points are read whole for the chart, in pages of the largest size.
const EQUITY_PAGE_SIZE = 200;
// A run has one point per decision and execution; this bounds a runaway read.
export const MAXIMUM_EQUITY_POINTS = 20_000;

export interface StartedRun {
  readonly job: Job;
  // An equal evaluation was already queued, running, or done.
  readonly deduplicated: boolean;
}

export interface ResearchClient {
  getStatus(signal?: AbortSignal): Promise<ResearchStatus>;
  // The growing lists page with keyset cursors: following next_cursor
  // never repeats or skips an entry; null starts from the first page.
  listExperiments(cursor: string | null, signal?: AbortSignal): Promise<ExperimentPage>;
  getExperiment(experimentId: string, signal?: AbortSignal): Promise<Experiment>;
  startRun(request: RunRequest): Promise<StartedRun>;
  listRuns(cursor: string | null, signal?: AbortSignal): Promise<JobPage>;
  getRun(jobId: string, signal?: AbortSignal): Promise<Job>;
  listResults(cursor: string | null, signal?: AbortSignal): Promise<ResultPage>;
  getResult(runId: string, signal?: AbortSignal): Promise<ResearchResult>;
  listTrades(runId: string, account: AccountName, cursor: string | null, signal?: AbortSignal): Promise<TradePage>;
  listDecisions(runId: string, cursor: string | null, signal?: AbortSignal): Promise<DecisionPage>;
  // Every point, in order.
  getEquity(runId: string, signal?: AbortSignal): Promise<readonly EquityPoint[]>;
}

const DEFAULT_RESEARCH_TRANSPORT: TransportOptions = {
  fetch: (input, init) => fetch(input, init),
  readTimeoutMs: 10_000,
  writeTimeoutMs: 20_000,
  identity: RESEARCH_IDENTITY,
};

function query(parameters: Record<string, string | null>): string {
  const search = new URLSearchParams();
  for (const [name, value] of Object.entries(parameters)) {
    if (value !== null) {
      search.set(name, value);
    }
  }
  const text = search.toString();
  return text === '' ? '' : `?${text}`;
}

export function createResearchClient(transport: TransportOptions = DEFAULT_RESEARCH_TRANSPORT): ResearchClient {
  const options = { ...transport, identity: transport.identity ?? RESEARCH_IDENTITY };

  async function get<Schema extends z.ZodType>(
    schema: Schema,
    path: string,
    signal?: AbortSignal,
  ): Promise<z.infer<Schema>> {
    const response = await sendRequest(options, { method: 'GET', path: RESEARCH_API + path, signal });
    if (response.status !== 200) {
      throw failureOf(response);
    }
    return decodeBody(schema, response);
  }

  return {
    getStatus: (signal) => get(StatusSchema, '/status', signal),
    listExperiments: (cursor, signal) =>
      get(ExperimentPageSchema, `/experiments${query({ limit: String(RESEARCH_PAGE_SIZE), cursor })}`, signal),
    getExperiment: async (experimentId, signal) =>
      (await get(ExperimentEnvelopeSchema, `/experiments/${encodeURIComponent(experimentId)}`, signal)).experiment,
    async startRun(request) {
      const response: HttpResponse = await sendRequest(options, {
        method: 'POST',
        path: `${RESEARCH_API}/runs`,
        body: request,
      });
      if (response.status !== 202 && response.status !== 200) {
        throw failureOf(response);
      }
      return decodeBody(StartedJobSchema, response);
    },
    listRuns: (cursor, signal) => get(JobPageSchema, `/runs${query({ limit: String(RESEARCH_PAGE_SIZE), cursor })}`, signal),
    getRun: async (jobId, signal) => (await get(JobEnvelopeSchema, `/runs/${encodeURIComponent(jobId)}`, signal)).run,
    listResults: (cursor, signal) =>
      get(ResultPageSchema, `/results${query({ limit: String(RESEARCH_PAGE_SIZE), cursor })}`, signal),
    getResult: async (runId, signal) =>
      (await get(ResultEnvelopeSchema, `/results/${encodeURIComponent(runId)}`, signal)).result,
    listTrades: (runId, account, cursor, signal) =>
      get(
        TradePageSchema,
        `/results/${encodeURIComponent(runId)}/trades${query({ account, limit: String(RESEARCH_PAGE_SIZE), cursor })}`,
        signal,
      ),
    listDecisions: (runId, cursor, signal) =>
      get(
        DecisionPageSchema,
        `/results/${encodeURIComponent(runId)}/decisions${query({ limit: String(RESEARCH_PAGE_SIZE), cursor })}`,
        signal,
      ),
    async getEquity(runId, signal) {
      const points: EquityPoint[] = [];
      let cursor: string | null = null;
      do {
        const page: z.infer<typeof EquityPageSchema> = await get(
          EquityPageSchema,
          `/results/${encodeURIComponent(runId)}/equity${query({ limit: String(EQUITY_PAGE_SIZE), cursor })}`,
          signal,
        );
        points.push(...page.points);
        cursor = page.next_cursor;
        if (points.length > MAXIMUM_EQUITY_POINTS) {
          throw new ChronosApiError({
            kind: 'unexpected_response',
            status: 200,
            detail: `the equity curve has more than ${String(MAXIMUM_EQUITY_POINTS)} points`,
          });
        }
      } while (cursor !== null);
      return points;
    },
  };
}
