// Runtime decoders for the model library of the research service
// (docs/research-service.md#model-library). As elsewhere, integers are
// strings and timestamps exact RFC 3339; a field the service does not send
// is never invented here.
import { z } from 'zod';

import { ComparisonJobSchema } from './comparisonSchemas';
import { ArtifactIdSchema, ClassSchema, JobIdSchema, JobStateSchema } from './researchSchemas';
import { TimestampSchema, UnsignedIntegerSchema } from './schemas';

export const ModelFamilySchema = z.enum(['logistic', 'majority', 'onnx']);
export type ModelFamily = z.infer<typeof ModelFamilySchema>;
// The families a verified experiment's model can be registered as.
export const ExperimentFamilySchema = z.enum(['logistic', 'majority']);
export type ExperimentFamily = z.infer<typeof ExperimentFamilySchema>;

export const LifecycleStateSchema = z.enum(['active', 'archived']);
export type LifecycleState = z.infer<typeof LifecycleStateSchema>;

export const LifecycleSchema = z.object({
  state: LifecycleStateSchema,
  changed_at: TimestampSchema.nullable(),
  reason: z.string().nullable(),
});

const NameSchema = z.object({ name: z.string(), version: z.string() });
export type ModelNameVersion = z.infer<typeof NameSchema>;

const PayloadSchema = z.object({
  name: z.string(),
  sha256: ArtifactIdSchema,
  size_bytes: UnsignedIntegerSchema,
});

// The information cutoff, with what it rests on. An unavailable cutoff has
// its reason and never a time.
export const CutoffSchema = z.discriminatedUnion('basis', [
  z.object({
    basis: z.literal('verified'),
    at: TimestampSchema,
    meaning: z.string(),
  }),
  z.object({
    basis: z.literal('declared'),
    at: TimestampSchema,
    meaning: z.string(),
  }),
  z.object({ basis: z.literal('unavailable'), reason: z.string() }),
]);
export type Cutoff = z.infer<typeof CutoffSchema>;

const TrainingSchema = z.object({
  information_cutoff: CutoffSchema,
  labels_available_through: TimestampSchema.optional(),
  rows: UnsignedIntegerSchema.optional(),
  class_counts: z.record(z.string(), UnsignedIntegerSchema).optional(),
  split: z.string().optional(),
});

const ExperimentProvenanceSchema = z.object({
  origin: z.literal('experiment'),
  experiment_id: ArtifactIdSchema,
  model: z.string(),
  sources: z.object({
    dataset_id: ArtifactIdSchema,
    feature_artifact_id: ArtifactIdSchema,
    label_artifact_id: ArtifactIdSchema,
    split_artifact_id: ArtifactIdSchema,
    stream: z.string(),
    horizon_ns: UnsignedIntegerSchema,
    validation_start_at: TimestampSchema,
    test_start_at: TimestampSchema,
  }),
  environment: z.record(z.string(), z.string()),
  code: z.record(z.string(), z.string()),
});

// What the uploader of an imported model declared, kept apart from what
// the sandbox validated. A missing cutoff is null, never zero.
const ImportProvenanceSchema = z.object({
  origin: z.literal('import'),
  declared: z.object({
    information_cutoff: TimestampSchema.nullable(),
    cutoff_covers: z.string().nullable(),
    training: z.string(),
    source: z.string(),
  }),
  manifest_sha256: ArtifactIdSchema,
  not_verified: z.record(z.string(), z.string()),
  validation: z.object({
    onnxruntime: z.string(),
    profile: z.object({
      operators: z.array(z.string()),
      nodes: UnsignedIntegerSchema,
      opset: UnsignedIntegerSchema,
      profile: z.object({ name: z.string(), version: z.string() }),
    }),
    sandbox_content_sha256: ArtifactIdSchema,
    smoke: z.object({
      corpus: z.string(),
      corpus_sha256: ArtifactIdSchema,
      outputs_sha256: ArtifactIdSchema,
      rows: UnsignedIntegerSchema,
    }),
  }),
});

export const ProvenanceSchema = z.discriminatedUnion('origin', [ExperimentProvenanceSchema, ImportProvenanceSchema]);
export type Provenance = z.infer<typeof ProvenanceSchema>;

const InputFieldSchema = z.object({
  name: z.string(),
  definition: z.string(),
  scale: UnsignedIntegerSchema,
  unit: z.string(),
});

export const ModelSummarySchema = z.object({
  model_version_id: ArtifactIdSchema,
  names: z.array(NameSchema),
  lifecycle: LifecycleSchema,
  family: ModelFamilySchema,
  instrument: z.string(),
  format: z.object({
    name: z.string(),
    version: z.string(),
    payload: z.string(),
  }),
  runtime: z.object({
    name: z.string(),
    version: z.string(),
    description: z.string(),
    class: z.string(),
  }),
  payload: PayloadSchema,
  inputs: z.object({
    name: z.string(),
    version: z.string(),
    conversion: z.string(),
    row: z.string(),
    fields: z.array(InputFieldSchema),
  }),
  output: z.object({
    kind: z.string(),
    class_order: z.array(ClassSchema),
    probabilities: z.string(),
    confidence_ppb: z.string(),
  }),
  target: z.object({
    label_set: z.string(),
    horizon_ns: UnsignedIntegerSchema,
    definition: z.string(),
  }),
  training: TrainingSchema,
  provenance: ProvenanceSchema,
  verification: z.record(z.string(), z.string()),
});
export type ModelSummary = z.infer<typeof ModelSummarySchema>;

const CompatibilitySchema = z.discriminatedUnion('status', [
  z.object({ status: z.literal('compatible'), requires: z.string() }),
  z.object({
    status: z.literal('unavailable'),
    reason: z.string(),
    detail: z.string(),
  }),
]);
export type Compatibility = z.infer<typeof CompatibilitySchema>;

export const SourceCheckSchema = z.discriminatedUnion('state', [
  z.object({
    experiment_id: ArtifactIdSchema,
    state: z.enum(['matches', 'missing', 'invalid', 'differs']),
    detail: z.string().optional(),
  }),
  z.object({ state: z.literal('not_applicable'), reason: z.string() }),
]);
export type SourceCheck = z.infer<typeof SourceCheckSchema>;

export const ModelDetailSchema = ModelSummarySchema.extend({
  compatibility: z.object({
    evaluation: CompatibilitySchema,
    deployment: CompatibilitySchema,
  }),
  limitations: z.array(z.string()),
  source_check: SourceCheckSchema,
});
export type ModelDetail = z.infer<typeof ModelDetailSchema>;
export const ModelEnvelopeSchema = z.object({ model: ModelDetailSchema });

export const ModelListingSchema = ModelSummarySchema.pick({
  model_version_id: true,
  names: true,
  lifecycle: true,
  family: true,
  instrument: true,
  payload: true,
  training: true,
}).extend({ origin: z.enum(['experiment', 'import']) });
export type ModelListing = z.infer<typeof ModelListingSchema>;

export const ModelPageSchema = z.object({
  models: z.array(ModelListingSchema),
  invalid_entries: z.array(z.object({ model_version_id: z.string(), reason: z.string() })),
  next_cursor: z.string().nullable(),
});
export type ModelPage = z.infer<typeof ModelPageSchema>;

export const RegistrationRequestSchema = z.object({
  experiment_id: ArtifactIdSchema,
  instrument: z.string(),
  model: ExperimentFamilySchema,
  name: z.string(),
  version: z.string(),
});
export type RegistrationRequest = z.infer<typeof RegistrationRequestSchema>;

// A job that publishes a model version: a registration or an import.
const PublicationJobFields = {
  job_id: JobIdSchema,
  state: JobStateSchema,
  created_at: TimestampSchema,
  started_at: TimestampSchema.nullable(),
  finished_at: TimestampSchema.nullable(),
  result: z
    .object({
      model_version_id: ArtifactIdSchema,
      outcome: z.enum(['published', 'already_present']),
      binding_outcome: z.enum(['published', 'already_present']),
    })
    .nullable(),
  // An import's refusal also names its reason, such as unsupported_operator.
  error: z
    .object({
      code: z.string(),
      message: z.string(),
      reason: z.string().optional(),
    })
    .nullable(),
  log: z.string(),
};

export const RegistrationJobSchema = z.object({
  ...PublicationJobFields,
  kind: z.literal('registration'),
  request: RegistrationRequestSchema,
});
export type RegistrationJob = z.infer<typeof RegistrationJobSchema>;

export const ImportJobSchema = z.object({
  ...PublicationJobFields,
  kind: z.literal('import'),
  request: z.object({
    import_id: JobIdSchema,
    instrument: z.string(),
    manifest_sha256: ArtifactIdSchema,
    model_sha256: ArtifactIdSchema,
    name: z.string(),
    version: z.string(),
  }),
});
export type ImportJob = z.infer<typeof ImportJobSchema>;

// A job of the model library: a registration, an import, or a comparison.
export const ModelJobSchema = z.discriminatedUnion('kind', [RegistrationJobSchema, ImportJobSchema, ComparisonJobSchema]);
export type ModelJob = z.infer<typeof ModelJobSchema>;

export const StartedRegistrationSchema = z.object({
  job: RegistrationJobSchema,
  deduplicated: z.boolean(),
});
export type StartedRegistration = z.infer<typeof StartedRegistrationSchema>;
export const ModelJobEnvelopeSchema = z.object({ job: ModelJobSchema });

// A staged import: the uploader's manifest as sent, and the model bytes once
// uploaded, by their checksum. Nothing has validated the model yet.
export const StagedImportSchema = z.object({
  import_id: JobIdSchema,
  created_at: TimestampSchema,
  manifest: z.record(z.string(), z.unknown()),
  manifest_sha256: ArtifactIdSchema,
  model: z
    .object({
      sha256: ArtifactIdSchema,
      size_bytes: UnsignedIntegerSchema,
      uploaded_at: TimestampSchema,
    })
    .nullable(),
});
export type StagedImport = z.infer<typeof StagedImportSchema>;
export const ImportDocumentSchema = z.object({
  import: StagedImportSchema,
  job: ImportJobSchema.nullable(),
});
export type ImportDocument = z.infer<typeof ImportDocumentSchema>;
export const StartedImportSchema = z.object({
  job: ImportJobSchema,
  deduplicated: z.boolean(),
});
export type StartedImport = z.infer<typeof StartedImportSchema>;

export const LifecycleChangeSchema = z.object({
  model_version_id: ArtifactIdSchema,
  lifecycle: LifecycleSchema,
});

// An uploader's chronos.model_import/1 manifest, read in the browser only to
// show it for review. The service checks it strictly; this decoder never
// replaces that check.
const TensorSchema = z.object({
  name: z.string(),
  dtype: z.string(),
  shape: z.array(z.string()),
});
export const ImportManifestSchema = z.object({
  schema: z.object({
    name: z.literal('chronos.model_import'),
    version: z.literal('1'),
  }),
  name: z.string(),
  version: z.string(),
  instrument: z.string(),
  inputs: z.object({ name: z.string(), version: z.string() }),
  input_tensor: TensorSchema,
  output_tensor: TensorSchema,
  class_order: z.array(z.string()),
  target: z.object({
    horizon_ns: UnsignedIntegerSchema,
    label_set: z.string(),
  }),
  declared: z.object({
    information_cutoff_ns: UnsignedIntegerSchema.nullable(),
    cutoff_covers: z.string().nullable(),
    training: z.string(),
    source: z.string(),
  }),
});
export type ImportManifest = z.infer<typeof ImportManifestSchema>;
