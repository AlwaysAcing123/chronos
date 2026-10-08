import { useId, useState } from 'react';

import type { Cutoff } from '../../api/modelSchemas';
import { CUTOFF_BASIS, FAMILY_LABEL, LIFECYCLE_LABEL, ORIGIN_LABEL, modelName } from '../../presentation/models';
import { shortId } from '../../presentation/research';
import type { LibraryFilter } from '../../state/models';
import { useModels } from '../../state/models';
import { modelsHref } from '../../state/route';
import historyStyles from '../history/history.module.css';
import { ListFooter } from '../research/ListFooter';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Unavailable } from '../ui/DecimalText';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';

const FILTERS: readonly {
  readonly value: LibraryFilter;
  readonly label: string;
}[] = [
  { value: 'active', label: 'Active' },
  { value: 'archived', label: 'Archived' },
  { value: 'all', label: 'All' },
];

export function CutoffText({ cutoff }: { readonly cutoff: Cutoff }) {
  const basis = CUTOFF_BASIS[cutoff.basis];
  return (
    <div>
      {cutoff.basis === 'unavailable' ? (
        <Unavailable>{`Unavailable: ${cutoff.reason.replaceAll('_', ' ')}`}</Unavailable>
      ) : (
        <MarketTime value={cutoff.at} stacked />
      )}
      <Badge tone={basis.tone}>{basis.label}</Badge>
    </div>
  );
}

// Every registered model version, active ones first by default.
export function ModelLibrary() {
  const [filter, setFilter] = useState<LibraryFilter>('active');
  const models = useModels(filter);
  const legendId = useId();
  return (
    <Panel title="Model versions" meta="Verified when read">
      <fieldset className={styles.choices} aria-describedby={legendId}>
        <legend id={legendId}>Show</legend>
        <div className={styles.inline}>
          {FILTERS.map((option) => (
            <label key={option.value} className={styles.choice}>
              <input
                type="radio"
                name="model-library-filter"
                value={option.value}
                checked={filter === option.value}
                onChange={() => {
                  setFilter(option.value);
                }}
              />
              {option.label}
            </label>
          ))}
        </div>
      </fieldset>
      <div className={styles.section}>
        {models.status === 'pending' ? (
          <p role="status">Loading model versions…</p>
        ) : models.data === undefined ? (
          <ResearchFailure
            error={models.error}
            title="The model library could not be read"
            onRetry={() => void models.refetch()}
            retrying={models.isFetching}
          />
        ) : (
          <>
            {models.data.invalid_entries.length > 0 && (
              <Alert tone="negative" title="Some model versions do not verify and cannot be used">
                <ul className={styles.list}>
                  {models.data.invalid_entries.map((entry) => (
                    <li key={entry.model_version_id}>
                      <Code>{shortId(entry.model_version_id)}</Code> {entry.reason}
                    </li>
                  ))}
                </ul>
              </Alert>
            )}
            {models.data.models.length === 0 ? (
              <div className={historyStyles.empty}>
                <p>{filter === 'archived' ? 'No archived model versions.' : 'No model versions are registered yet.'}</p>
                {filter !== 'archived' && (
                  <p className={styles.note}>Register a fitted model of a verified experiment with the form beside.</p>
                )}
              </div>
            ) : (
              <div className={historyStyles.scroll}>
                <table className={historyStyles.table}>
                  <caption className="visually-hidden">Registered model versions</caption>
                  <thead>
                    <tr>
                      <th scope="col">Model</th>
                      <th scope="col">Instrument</th>
                      <th scope="col">Origin</th>
                      <th scope="col">Information cutoff (UTC)</th>
                      <th scope="col">State</th>
                    </tr>
                  </thead>
                  <tbody>
                    {models.data.models.map((model) => {
                      const lifecycle = LIFECYCLE_LABEL[model.lifecycle.state];
                      return (
                        <tr key={model.model_version_id}>
                          <td data-label="Model">
                            <div>
                              <a href={modelsHref(model.model_version_id)}>{modelName(model)}</a>
                              <span className={styles.reasonCode}>{FAMILY_LABEL[model.family]}</span>
                              <span className={styles.reasonCode}>{shortId(model.model_version_id)}</span>
                            </div>
                          </td>
                          <td data-label="Instrument" className="numeric">
                            {model.instrument}
                          </td>
                          <td data-label="Origin">{ORIGIN_LABEL[model.origin]}</td>
                          <td data-label="Information cutoff (UTC)">
                            <CutoffText cutoff={model.training.information_cutoff} />
                          </td>
                          <td data-label="State">
                            <Badge tone={lifecycle.tone}>{lifecycle.label}</Badge>
                          </td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
                <ListFooter
                  shown={models.data.models.length}
                  noun="model versions"
                  hasMore={models.hasNextPage}
                  loading={models.isFetchingNextPage}
                  failed={models.isFetchNextPageError}
                  onLoadMore={() => void models.fetchNextPage()}
                />
              </div>
            )}
          </>
        )}
      </div>
    </Panel>
  );
}
