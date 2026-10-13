// The one boundary between the application and the local research service.
// Like the trading client, every response is decoded before it reaches
// application state, and a response without the service's header is not
// trusted as one.
import type { z } from 'zod';

import { ChronosApiError } from './errors';
import type { HttpResponse, ResponseIdentity, TransportOptions } from './http';
import { decodeBody, failureOf, sendRequest } from './http';
import type { ComparisonDetail, ComparisonPage, ComparisonRequest, StartedComparison } from './comparisonSchemas';
import type {
  DiagnosticsDetail,
  DiagnosticsJob,
  DiagnosticsPage,
  EpisodePage,
  FillPage,
  SimulatedAccount,
  StartedDiagnostics,
} from './diagnosticsSchemas';
import {
  DiagnosticsEnvelopeSchema,
  DiagnosticsJobEnvelopeSchema,
  DiagnosticsPageSchema,
  EpisodePageSchema,
  FillPageSchema,
  StartedDiagnosticsSchema,
} from './diagnosticsSchemas';
import { ComparisonEnvelopeSchema, ComparisonPageSchema, StartedComparisonSchema } from './comparisonSchemas';
import type {
  CollectionPage,
  Plan,
  StartedPlan,
  StartedStudy,
  StudyDetail,
  StudyJobPage,
  StudyPage,
  WalkForwardJob,
  WalkForwardRequest,
} from './walkForwardSchemas';
import {
  CollectionPageSchema,
  PlanEnvelopeSchema,
  StartedPlanSchema,
  StartedStudySchema,
  StudyEnvelopeSchema,
  StudyJobPageSchema,
  StudyPageSchema,
  WalkForwardJobEnvelopeSchema,
} from './walkForwardSchemas';
import type {
  ImportDocument,
  LifecycleState,
  ModelDetail,
  ModelJob,
  ModelPage,
  RegistrationRequest,
  StartedImport,
  StartedRegistration,
} from './modelSchemas';
import {
  ImportDocumentSchema,
  LifecycleChangeSchema,
  ModelEnvelopeSchema,
  ModelJobEnvelopeSchema,
  ModelPageSchema,
  StartedImportSchema,
  StartedRegistrationSchema,
} from './modelSchemas';
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
  // The model library.
  listModels(state: LifecycleState | 'all', cursor: string | null, signal?: AbortSignal): Promise<ModelPage>;
  getModel(modelVersionId: string, signal?: AbortSignal): Promise<ModelDetail>;
  startRegistration(request: RegistrationRequest): Promise<StartedRegistration>;
  // A registration or import job.
  getModelJob(jobId: string, signal?: AbortSignal): Promise<ModelJob>;
  setModelLifecycle(modelVersionId: string, state: LifecycleState, reason: string): Promise<void>;
  // An ONNX import: the manifest file's own text, then the model file's own
  // bytes, then validation of exactly the bytes whose checksum was reviewed.
  stageImport(manifestText: string): Promise<ImportDocument>;
  uploadImportModel(importId: string, model: Uint8Array<ArrayBuffer>): Promise<ImportDocument>;
  startImportValidation(importId: string, modelSha256: string): Promise<StartedImport>;
  getImport(importId: string, signal?: AbortSignal): Promise<ImportDocument>;
  // Comparisons of registered model versions under one frozen spec.
  listComparisons(cursor: string | null, signal?: AbortSignal): Promise<ComparisonPage>;
  getComparison(comparisonId: string, signal?: AbortSignal): Promise<ComparisonDetail>;
  startComparison(request: ComparisonRequest): Promise<StartedComparison>;
  // Walk-forward evaluation: verified collections, a Preview job that
  // publishes the frozen plan of exactly the request, and a study job that
  // names only that plan's ID.
  listCollections(cursor: string | null, signal?: AbortSignal): Promise<CollectionPage>;
  previewWalkForward(request: WalkForwardRequest): Promise<StartedPlan>;
  getWalkForwardPlan(planId: string, signal?: AbortSignal): Promise<Plan>;
  startWalkForwardStudy(planId: string): Promise<StartedStudy>;
  listWalkForwardStudies(cursor: string | null, signal?: AbortSignal): Promise<StudyPage>;
  getWalkForwardStudy(studyId: string, signal?: AbortSignal): Promise<StudyDetail>;
  getWalkForwardJob(jobId: string, signal?: AbortSignal): Promise<WalkForwardJob>;
  listStudyJobs(cursor: string | null, signal?: AbortSignal): Promise<StudyJobPage>;
  // Strategy diagnostics of existing studies: generated only by a job, read
  // verified; their fill and episode rows are paged per run.
  startDiagnostics(studyIds: readonly string[]): Promise<StartedDiagnostics>;
  getDiagnosticsJob(jobId: string, signal?: AbortSignal): Promise<DiagnosticsJob>;
  listDiagnostics(studyId: string | null, cursor: string | null, signal?: AbortSignal): Promise<DiagnosticsPage>;
  getDiagnostics(diagnosticId: string, signal?: AbortSignal): Promise<DiagnosticsDetail>;
  listDiagnosticFills(
    diagnosticId: string,
    runId: string,
    account: SimulatedAccount,
    cursor: string | null,
    signal?: AbortSignal,
  ): Promise<FillPage>;
  listDiagnosticEpisodes(
    diagnosticId: string,
    runId: string,
    account: SimulatedAccount,
    cursor: string | null,
    signal?: AbortSignal,
  ): Promise<EpisodePage>;
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

  async function send<Schema extends z.ZodType>(
    schema: Schema,
    request: Parameters<typeof sendRequest>[1],
    accepted: readonly number[],
  ): Promise<z.infer<Schema>> {
    const response: HttpResponse = await sendRequest(options, { ...request, path: RESEARCH_API + request.path });
    if (!accepted.includes(response.status)) {
      throw failureOf(response);
    }
    return decodeBody(schema, response);
  }

  const post = <Schema extends z.ZodType>(schema: Schema, path: string, body: unknown, accepted: readonly number[]) =>
    send(schema, { method: 'POST', path, body }, accepted);

  return {
    getStatus: (signal) => get(StatusSchema, '/status', signal),
    listModels: (state, cursor, signal) =>
      get(ModelPageSchema, `/models${query({ limit: String(RESEARCH_PAGE_SIZE), cursor, state })}`, signal),
    getModel: async (modelVersionId, signal) =>
      (await get(ModelEnvelopeSchema, `/models/${encodeURIComponent(modelVersionId)}`, signal)).model,
    startRegistration: (request) => post(StartedRegistrationSchema, '/models', request, [200, 202]),
    getModelJob: async (jobId, signal) =>
      (await get(ModelJobEnvelopeSchema, `/jobs/${encodeURIComponent(jobId)}`, signal)).job,
    async setModelLifecycle(modelVersionId, state, reason) {
      await post(
        LifecycleChangeSchema,
        `/models/${encodeURIComponent(modelVersionId)}/lifecycle`,
        { state, reason },
        [200],
      );
    },
    stageImport: (manifestText) =>
      send(
        ImportDocumentSchema,
        { method: 'POST', path: '/imports', raw: { contentType: 'application/json', data: manifestText } },
        [201],
      ),
    uploadImportModel: (importId, model) =>
      send(
        ImportDocumentSchema,
        {
          method: 'PUT',
          path: `/imports/${encodeURIComponent(importId)}/model`,
          raw: { contentType: 'application/octet-stream', data: model },
        },
        [200, 201],
      ),
    startImportValidation: (importId, modelSha256) =>
      post(StartedImportSchema, `/imports/${encodeURIComponent(importId)}/validation`, { model_sha256: modelSha256 }, [
        200, 202,
      ]),
    getImport: (importId, signal) => get(ImportDocumentSchema, `/imports/${encodeURIComponent(importId)}`, signal),
    listComparisons: (cursor, signal) =>
      get(ComparisonPageSchema, `/comparisons${query({ limit: String(RESEARCH_PAGE_SIZE), cursor })}`, signal),
    getComparison: (comparisonId, signal) =>
      get(ComparisonEnvelopeSchema, `/comparisons/${encodeURIComponent(comparisonId)}`, signal),
    startComparison: (request) => post(StartedComparisonSchema, '/comparisons', request, [200, 202]),
    listCollections: (cursor, signal) =>
      get(CollectionPageSchema, `/collections${query({ limit: String(RESEARCH_PAGE_SIZE), cursor })}`, signal),
    previewWalkForward: (request) => post(StartedPlanSchema, '/walk-forward/plans', request, [200, 202]),
    getWalkForwardPlan: async (planId, signal) =>
      (await get(PlanEnvelopeSchema, `/walk-forward/plans/${encodeURIComponent(planId)}`, signal)).plan,
    startWalkForwardStudy: (planId) =>
      post(StartedStudySchema, '/walk-forward/studies', { plan_id: planId }, [200, 202]),
    listWalkForwardStudies: (cursor, signal) =>
      get(StudyPageSchema, `/walk-forward/studies${query({ limit: String(RESEARCH_PAGE_SIZE), cursor })}`, signal),
    getWalkForwardStudy: (studyId, signal) =>
      get(StudyEnvelopeSchema, `/walk-forward/studies/${encodeURIComponent(studyId)}`, signal),
    getWalkForwardJob: async (jobId, signal) =>
      (await get(WalkForwardJobEnvelopeSchema, `/jobs/${encodeURIComponent(jobId)}`, signal)).job,
    listStudyJobs: (cursor, signal) =>
      get(
        StudyJobPageSchema,
        `/jobs${query({ kind: 'walk_forward', limit: String(RESEARCH_PAGE_SIZE), cursor })}`,
        signal,
      ),
    startDiagnostics: (studyIds) =>
      post(StartedDiagnosticsSchema, '/strategy-diagnostics', { study_ids: studyIds }, [200, 202]),
    getDiagnosticsJob: async (jobId, signal) =>
      (await get(DiagnosticsJobEnvelopeSchema, `/jobs/${encodeURIComponent(jobId)}`, signal)).job,
    listDiagnostics: (studyId, cursor, signal) =>
      get(
        DiagnosticsPageSchema,
        `/strategy-diagnostics${query({ limit: String(RESEARCH_PAGE_SIZE), cursor, study: studyId })}`,
        signal,
      ),
    getDiagnostics: (diagnosticId, signal) =>
      get(DiagnosticsEnvelopeSchema, `/strategy-diagnostics/${encodeURIComponent(diagnosticId)}`, signal),
    listDiagnosticFills: (diagnosticId, runId, account, cursor, signal) =>
      get(
        FillPageSchema,
        `/strategy-diagnostics/${encodeURIComponent(diagnosticId)}/fills${query({
          run: runId,
          account,
          limit: String(RESEARCH_PAGE_SIZE),
          cursor,
        })}`,
        signal,
      ),
    listDiagnosticEpisodes: (diagnosticId, runId, account, cursor, signal) =>
      get(
        EpisodePageSchema,
        `/strategy-diagnostics/${encodeURIComponent(diagnosticId)}/episodes${query({
          run: runId,
          account,
          limit: String(RESEARCH_PAGE_SIZE),
          cursor,
        })}`,
        signal,
      ),
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
