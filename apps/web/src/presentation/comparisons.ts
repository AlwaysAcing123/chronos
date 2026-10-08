// Words for comparisons. A member is completed, refused, or failed, and each
// label says which; a refusal or failure keeps its reason.
import type { MemberState } from '../api/comparisonSchemas';
import type { Labelled } from './vocabulary';

export const MEMBER_STATE: Readonly<Record<MemberState, Labelled>> = {
  completed: { label: 'Completed', tone: 'positive' },
  refused: { label: 'Refused', tone: 'caution' },
  failed: { label: 'Failed', tone: 'negative' },
};

// What a reason code means. Codes not named here are an invalid prediction's
// reasons; the page shows every code beside its meaning.
const REASON: Readonly<Record<string, string>> = {
  archived: 'Archived versions cannot be chosen',
  instrument_mismatch: 'Predicts another instrument',
  horizon_mismatch: 'Predicts another horizon than the split’s labels',
  cutoff_after_window_start: 'Its information cutoff is after the window starts',
  cutoff_unavailable: 'Its information cutoff is unknown',
  artifact_invalid: 'The version does not verify',
  schedule_failed: 'Its predictions could not be scheduled',
  schedule_not_reproduced: 'Its schedule did not rebuild identically',
  schedule_not_published: 'Its schedule could not be stored',
  run_failed: 'The strategy run failed',
  replay_failed: 'The run did not replay identically',
};

export function memberReason(reason: string): string {
  if (reason.startsWith('sandbox_unavailable')) {
    return 'The ONNX sandbox is unavailable';
  }
  if (reason.startsWith('sandbox_')) {
    return 'The ONNX sandbox failed';
  }
  return REASON[reason] ?? 'Its predictions are not valid';
}

// Why the split left a row of the window out of scoring, such as
// `label_unavailable:no_future_observation`.
export function notScoredReason(reason: string): string {
  return reason.replace(':', ': ').replaceAll('_', ' ');
}

export function artifactCheck(state: string | undefined): Labelled {
  if (state === undefined) {
    return { label: 'Not produced', tone: 'neutral' };
  }
  return state === 'verified' ? { label: 'Verifies', tone: 'positive' } : { label: 'Does not verify', tone: 'negative' };
}
