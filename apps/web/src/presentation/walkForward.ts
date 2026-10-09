// Words for walk-forward evaluation. Every state is named in text beside its
// colour, and every reason code keeps its meaning beside it.
import type { Cell, Evidence, Progress, Study, StudyArtifacts, StudyModel } from '../api/walkForwardSchemas';
import type { ModelName } from '../api/researchSchemas';
import { MODEL_LABEL } from './research';
import type { Labelled } from './vocabulary';

export const EVIDENCE: Readonly<Record<Evidence, Labelled>> = {
  synthetic: { label: 'Synthetic recordings', tone: 'caution' },
  observed: { label: 'Observed provider data', tone: 'info' },
};

export const STUDY_STATUS: Readonly<Record<'complete' | 'partial', Labelled>> = {
  complete: { label: 'Complete', tone: 'positive' },
  partial: { label: 'Partial', tone: 'caution' },
};

export const MODEL_STATE: Readonly<Record<StudyModel['state'], Labelled>> = {
  completed: { label: 'Completed', tone: 'positive' },
  unevaluable: { label: 'Unevaluable', tone: 'caution' },
  failed: { label: 'Failed', tone: 'negative' },
  unavailable: { label: 'Unavailable', tone: 'neutral' },
};

export const CELL_STATE: Readonly<Record<Cell['state'], Labelled>> = {
  completed: { label: 'Completed', tone: 'positive' },
  failed: { label: 'Failed', tone: 'negative' },
  unavailable: { label: 'Not run', tone: 'neutral' },
};

const REASON: Readonly<Record<string, string>> = {
  no_training_rows: 'No training row was eligible',
  insufficient_class_support: 'Too few training rows of some class to fit',
  did_not_converge: 'The fit did not converge',
  fit_warning: 'The fit raised a warning',
  model_unevaluable: 'Its model could not be fitted, so nothing ran',
  schedule_failed: 'Its predictions could not be scheduled',
  fold_failed: 'The fold could not be fitted from its sources',
  run_failed: 'The strategy run failed',
  replay_failed: 'The run did not replay identically',
  run_mismatch: 'The run was not of this schedule and scenario',
  labels_unavailable: 'The evaluation labels could not be read',
  scoring_failed: 'The predictions could not be scored',
  source_changed: 'A training source no longer gives the planned rows',
  environment_differs: 'This environment fits differently from the plan’s',
  not_reproduced: 'Rebuilding gave different bytes',
  artifact_invalid: 'An artifact does not verify',
  insufficient_coverage: 'Not enough independent recordings',
  folds_missing: 'Not every planned fold has a valued result',
};

/** What a reason code means, such as `valuation_unavailable:strategy:missing_mark`. */
export function studyReason(reason: string): string {
  const [head, ...rest] = reason.split(':');
  if (head === 'valuation_unavailable') {
    return `A final valuation is unavailable (${rest.join(', ').replaceAll('_', ' ')})`;
  }
  if (head === 'cell_failed' || head === 'cell_unavailable') {
    return studyReason(rest.join(':'));
  }
  return REASON[head ?? ''] ?? reason.replaceAll('_', ' ');
}

export function scenarioLabel(name: string): string {
  if (name === 'base') {
    return 'Base';
  }
  if (name === 'stress') {
    return 'Stress';
  }
  return name === 'zero_fee' ? 'Zero fee' : name.replaceAll('_', ' ');
}

/** A missing item, such as `cell:logistic:stress`. */
export function missingItem(item: string): string {
  const [kind = '', model, scenario] = item.split(':');
  const name = model === 'logistic' || model === 'majority' ? MODEL_LABEL[model] : (model ?? '');
  switch (kind) {
    case 'fold_model':
      return 'Fold models';
    case 'model':
      return name;
    case 'scores':
      return `${name}: prediction scores`;
    case 'cell':
      return `${name}: ${scenarioLabel(scenario ?? '')} scenario`;
    default:
      return item;
  }
}

const STEP: Readonly<Record<NonNullable<Progress['step']>, string>> = {
  fit: 'fitting fresh models on its training recordings',
  schedule: 'predicting every evaluation row',
  run: 'running the engine',
  score: 'scoring predictions against labels',
};

/** A study job's latest report, in words. */
export function progressText(progress: Progress | null): string {
  if (progress === null) {
    return 'Waiting to start';
  }
  switch (progress.stage) {
    case 'verify_sources':
      return 'Verifying every recording’s sources again with the C++ tools';
    case 'aggregate':
      return 'Aggregating the folds';
    case 'publish':
      return 'Publishing the study';
    case 'fold': {
      const where = `Fold ${progress.fold ?? '?'} of ${progress.folds ?? '?'}`;
      const step = progress.step === undefined ? '' : STEP[progress.step];
      const model: ModelName | undefined = progress.model;
      const who =
        progress.step === 'run' && model !== undefined
          ? `: ${MODEL_LABEL[model]}, ${scenarioLabel(progress.scenario ?? '')} scenario`
          : progress.step === 'schedule' && model !== undefined
            ? `: ${MODEL_LABEL[model]}`
            : '';
      return `${where}: ${step}${who}`;
    }
  }
}

export function foldLabel(index: string, stream: string): string {
  return `Fold ${index} · ${stream}`;
}

// --- Verification read again now ---------------------------------------------------------------------------

/** One artifact's check, read again by the service: it verifies, it does
 * not (with why), or the service reported no check for it. */
export type Check =
  | { readonly state: 'verified' }
  | { readonly state: 'invalid'; readonly reason: string }
  | { readonly state: 'unchecked' };

export function checkOf(value: string | undefined): Check {
  if (value === undefined) {
    return { state: 'unchecked' };
  }
  return value === 'verified' ? { state: 'verified' } : { state: 'invalid', reason: value.replace(/^invalid: /, '') };
}

export const CHECK: Readonly<Record<Check['state'], Labelled>> = {
  verified: { label: 'Verifies', tone: 'positive' },
  invalid: { label: 'Does not verify', tone: 'negative' },
  unchecked: { label: 'Not checked', tone: 'caution' },
};

export interface Verification {
  /** Each completed run's own check, by `fold|model|scenario`. */
  readonly runs: ReadonlyMap<string, Check>;
  /** Whether a completed run's figures rest only on verified artifacts:
   * its fold models, its schedule, and itself, by `fold|model|scenario`. */
  readonly trusted: ReadonlyMap<string, boolean>;
  /** Each completed model's schedule check, by `fold|model`. */
  readonly schedules: ReadonlyMap<string, Check>;
  /** Every artifact that does not verify or was not checked, in words. */
  readonly problems: readonly string[];
}

function problem(check: Check, what: string): string | null {
  if (check.state === 'verified') {
    return null;
  }
  return `${what}: ${check.state === 'invalid' ? check.reason : 'not checked'}`;
}

/** What of a study still verifies, from the checks its detail route made
 * on this read: fold models, schedules, and runs. */
export function studyVerification(study: Study, artifacts: StudyArtifacts): Verification {
  const runs = new Map<string, Check>();
  const trusted = new Map<string, boolean>();
  const schedules = new Map<string, Check>();
  const problems: string[] = [];
  for (const fold of study.folds) {
    const checks = artifacts.find((entry) => entry.fold === fold.index);
    const foldModel = fold.fold_model.state === 'completed' ? checkOf(checks?.fold_model) : null;
    const foldProblem = foldModel === null ? null : problem(foldModel, `Fold ${fold.index} fold models`);
    if (foldProblem !== null) {
      problems.push(foldProblem);
    }
    for (const name of ['majority', 'logistic'] as const) {
      const model = fold.models[name];
      if (model.state !== 'completed') {
        continue;
      }
      const where = `Fold ${fold.index}, ${MODEL_LABEL[name]}`;
      const schedule = checkOf(checks?.models[name]?.schedule);
      schedules.set(`${fold.index}|${name}`, schedule);
      const scheduleProblem = problem(schedule, `${where} schedule`);
      if (scheduleProblem !== null) {
        problems.push(scheduleProblem);
      }
      for (const cell of model.cells) {
        if (cell.state !== 'completed') {
          continue;
        }
        const key = `${fold.index}|${name}|${cell.scenario}`;
        const run = checkOf(checks?.models[name]?.runs[cell.scenario]);
        runs.set(key, run);
        const runProblem = problem(run, `${where}, ${scenarioLabel(cell.scenario)} scenario run`);
        if (runProblem !== null) {
          problems.push(runProblem);
        }
        trusted.set(key, foldProblem === null && scheduleProblem === null && runProblem === null);
      }
    }
  }
  return { runs, trusted, schedules, problems };
}
