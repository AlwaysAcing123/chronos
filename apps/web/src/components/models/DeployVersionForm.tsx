import { useId, useState } from 'react';

import type { RegisteredDeploymentRequest } from '../../api/deployments';
import { isValidIdentifier } from '../../api/identifiers';
import type { ModelDetail } from '../../api/modelSchemas';
import { CUTOFF_BASIS, FAMILY_LABEL, modelName } from '../../presentation/models';
import { useCreateDeployment, useDeploymentClient } from '../../state/deployments';
import { useResearchStatus } from '../../state/research';
import { researchHref } from '../../state/route';
import {
  DEPLOYMENT_RULES,
  DeploymentCreationNotice,
  FIELDS,
  initialDraft,
  rejectedField,
} from '../research/DeploymentForm';
import type { Draft } from '../research/DeploymentForm';
import styles from '../research/research.module.css';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';

// Creates a paper deployment, stopped, of exactly this version, named by its
// ID. The server's worker verifies the version again before creating it, and
// again at every Start; nothing trades until Start.
export function DeployVersionForm({ model }: { readonly model: ModelDetail }) {
  const formId = useId();
  const client = useDeploymentClient();
  const status = useResearchStatus();
  const [draft, setDraft] = useState<Draft>(() => initialDraft(status.data?.defaults));
  const [review, setReview] = useState<RegisteredDeploymentRequest | null>(null);
  const create = useCreateDeployment();
  if (client === null || model.compatibility.deployment.status !== 'compatible') {
    return null;
  }
  const cutoff = model.training.information_cutoff;
  const blank = FIELDS.find((field) => draft[field.key].trim() === '');
  const badIdentifier =
    !isValidIdentifier(draft.deployment_id) || draft.deployment_id.length > 32 || !isValidIdentifier(draft.account_id);
  const rejected = rejectedField(create.error);
  return (
    <Panel title="Deploy this version to paper" meta={`${model.instrument} · ${FAMILY_LABEL[model.family]}`}>
      <form
        className={styles.review}
        onSubmit={(event) => {
          event.preventDefault();
          if (create.isPending) return;
          if (review === null) {
            if (blank !== undefined || badIdentifier) return;
            setReview({ ...draft, instrument: model.instrument, model_version_id: model.model_version_id });
            return;
          }
          create.mutate(review, {
            onSuccess: () => {
              setReview(null);
            },
          });
        }}
      >
        {cutoff.basis === 'declared' && (
          <Alert tone="caution" title="Its cutoff is declared, not verified">
            <p>
              The deployment decides only on market data received at or after the cutoff its uploader declared. Chronos
              did not verify that the model never saw later data.
            </p>
          </Alert>
        )}
        {review === null ? (
          <div className={styles.fields}>
            {FIELDS.map((field) => {
              const id = `${formId}-${field.key}`;
              const hint = field.hint?.(draft[field.key]) ?? null;
              return (
                <div key={field.key} className={styles.field}>
                  <label htmlFor={id}>{field.label}</label>
                  <input
                    id={id}
                    autoComplete="off"
                    spellCheck={false}
                    inputMode={field.key.endsWith('_id') ? 'text' : 'decimal'}
                    value={draft[field.key]}
                    aria-invalid={rejected === field.key}
                    aria-describedby={hint === null ? undefined : `${id}-hint`}
                    onChange={(event) => {
                      setDraft({ ...draft, [field.key]: event.target.value });
                    }}
                  />
                  {hint !== null && (
                    <span id={`${id}-hint`} className={styles.fieldHint}>
                      {hint}
                    </span>
                  )}
                </div>
              );
            })}
          </div>
        ) : (
          <div className={styles.review} aria-label="Review the deployment">
            <Facts
              facts={[
                { label: 'Model', value: modelName(model) },
                { label: 'Model version ID', value: <Code>{review.model_version_id}</Code> },
                { label: 'Instrument', value: review.instrument },
                {
                  label: 'Decides on data from (UTC)',
                  value:
                    cutoff.basis === 'unavailable' ? null : (
                      <>
                        <MarketTime value={cutoff.at} stacked />
                        <Badge tone={CUTOFF_BASIS[cutoff.basis].tone}>{CUTOFF_BASIS[cutoff.basis].label}</Badge>
                      </>
                    ),
                },
                ...FIELDS.map((field) => ({ label: field.label, value: review[field.key] })),
              ]}
            />
            <div>
              <h3 className={styles.sectionTitle}>What the deployment does</h3>
              <ul className={styles.list}>
                {DEPLOYMENT_RULES.map((text) => (
                  <li key={text}>{text}</li>
                ))}
                <li>
                  It serves exactly this version, by its ID: a later version under the same name never replaces it.
                  {model.family === 'onnx' && ' The model runs only in the ONNX sandbox, never in the server.'}
                </li>
              </ul>
            </div>
          </div>
        )}
        <DeploymentCreationNotice error={create.error} created={review === null ? create.data : undefined} />
        {create.data !== undefined && review === null && (
          <p className={styles.note}>
            <a href={researchHref()}>Open Paper deployments</a> to start it.
          </p>
        )}
        <div className={styles.actions}>
          {review !== null ? (
            <>
              <Button type="submit" variant="primary" disabled={create.isPending}>
                {create.isPending ? 'Verifying the model…' : 'Create deployment'}
              </Button>
              <Button
                disabled={create.isPending}
                onClick={() => {
                  setReview(null);
                }}
              >
                Edit
              </Button>
            </>
          ) : (
            <Button type="submit" variant="primary" disabled={blank !== undefined || badIdentifier}>
              Review deployment
            </Button>
          )}
        </div>
        {review === null && <p className={styles.note}>Simulated orders only; nothing is ever sent to a broker.</p>}
      </form>
    </Panel>
  );
}
