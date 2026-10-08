// Words for the model library. Artifact integrity, inference compatibility,
// and training provenance are different claims, and each label says which.
import type { Cutoff, LifecycleState, ModelFamily, ModelListing, SourceCheck } from '../api/modelSchemas';
import type { Timestamp } from '../api/schemas';
import { TimestampSchema } from '../api/schemas';
import { MODEL_LABEL, shortId } from './research';
import type { Labelled } from './vocabulary';

export const FAMILY_LABEL: Readonly<Record<ModelFamily, string>> = {
  ...MODEL_LABEL,
  onnx: 'Imported ONNX classifier',
};

/** A family's label, or the family itself when it is not one Chronos names. */
export function familyLabel(family: string): string {
  return Object.hasOwn(FAMILY_LABEL, family) ? FAMILY_LABEL[family as ModelFamily] : family;
}

export const LIFECYCLE_LABEL: Readonly<Record<LifecycleState, Labelled>> = {
  active: { label: 'Active', tone: 'neutral' },
  archived: { label: 'Archived', tone: 'caution' },
};

export const ORIGIN_LABEL: Readonly<Record<ModelListing['origin'], string>> = {
  experiment: 'Verified experiment',
  import: 'Import (declared provenance)',
};

export const CUTOFF_BASIS: Readonly<Record<Cutoff['basis'], Labelled>> = {
  verified: { label: 'Verified by Chronos', tone: 'positive' },
  declared: { label: 'Declared by uploader, not verified', tone: 'caution' },
  unavailable: { label: 'Unavailable', tone: 'negative' },
};

export const SOURCE_CHECK: Readonly<Record<SourceCheck['state'], Labelled>> = {
  matches: { label: 'Source experiment matches', tone: 'positive' },
  missing: { label: 'Source experiment missing', tone: 'caution' },
  invalid: { label: 'Source experiment does not verify', tone: 'negative' },
  differs: { label: 'Source experiment differs', tone: 'negative' },
  not_applicable: { label: 'No source experiment', tone: 'neutral' },
};

// What each verification statement is about, in the order shown.
export const VERIFICATION_LABEL: Readonly<Record<string, string>> = {
  experiment: 'Source experiment',
  sources: 'Source data',
  payload: 'Parameters',
  cutoff: 'Information cutoff',
  artifact: 'Model bytes',
  isolation: 'Isolation',
  profile: 'Profile',
  smoke_test: 'Smoke test',
  outputs: 'Outputs',
};

// What an import's uploader stated and Chronos could not check.
export const NOT_VERIFIED_LABEL: Readonly<Record<string, string>> = {
  training: 'Training',
  target: 'Target',
  cutoff: 'Information cutoff',
};

/** Service statements are lowercase clauses; shown alone, each reads as a sentence. */
export function sentence(text: string): string {
  const trimmed = text.trim();
  const capitalized = trimmed.charAt(0).toUpperCase() + trimmed.slice(1);
  return /[.!?]$/.test(capitalized) ? capitalized : `${capitalized}.`;
}

/** Verification statements in a fixed order, known kinds first. */
export function orderedStatements(statements: Readonly<Record<string, string>>): readonly [string, string][] {
  const known = Object.keys(VERIFICATION_LABEL);
  return Object.entries(statements).sort(([left], [right]) => {
    const order = (key: string) => (known.includes(key) ? known.indexOf(key) : known.length);
    return order(left) - order(right) || left.localeCompare(right);
  });
}

/** A version's human name, or its short ID when it has none. */
export function modelName(model: Pick<ModelListing, 'names' | 'model_version_id'>): string {
  const first = model.names[0];
  return first === undefined ? shortId(model.model_version_id) : `${first.name} ${first.version}`;
}

/** Exact RFC 3339 UTC time of integer nanoseconds since the epoch, or null when out of range. */
export function timestampOfNanoseconds(nanoseconds: string): Timestamp | null {
  if (!/^(0|[1-9][0-9]{0,18})$/.test(nanoseconds)) {
    return null;
  }
  const total = BigInt(nanoseconds);
  const milliseconds = Number(total / 1_000_000n);
  const date = new Date(milliseconds);
  if (Number.isNaN(date.getTime())) {
    return null;
  }
  const fraction = (total % 1_000_000_000n).toString().padStart(9, '0');
  const parsed = TimestampSchema.safeParse(`${date.toISOString().slice(0, 19)}.${fraction}Z`);
  return parsed.success ? parsed.data : null;
}

/** "1048576" bytes as "1 MiB"; smaller sizes in KiB with one decimal. */
export function byteSize(bytes: number): string {
  return bytes >= 1024 * 1024 && bytes % (1024 * 1024) === 0
    ? `${String(bytes / 1024 / 1024)} MiB`
    : `${(bytes / 1024).toFixed(1)} KiB`;
}
