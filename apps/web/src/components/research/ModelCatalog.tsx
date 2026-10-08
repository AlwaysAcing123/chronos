import type { UseInfiniteQueryResult } from '@tanstack/react-query';
import { useId } from 'react';

import type { Experiment, ExperimentInstrument, ExperimentPage, PredictionMetrics } from '../../api/researchSchemas';
import { MODEL_LABEL, WINDOW_LABEL, shortId } from '../../presentation/research';
import historyStyles from '../history/history.module.css';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Unavailable } from '../ui/DecimalText';
import { Facts } from '../ui/Facts';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';
import { ListFooter } from './ListFooter';
import { MetricValue } from './MetricValue';
import { ResearchFailure } from './Research';
import styles from './research.module.css';

export interface Selection {
  readonly experimentId: string;
  readonly instrument: string;
}

/** The first experiment and its first instrument with a fitted model. */
export function defaultSelection(page: ExperimentPage | undefined): Selection | null {
  const experiment = page?.experiments[0];
  if (experiment === undefined) {
    return null;
  }
  const instrument =
    experiment.instruments.find((entry) => entry.models.logistic.status === 'fitted') ?? experiment.instruments[0];
  return instrument === undefined ? null : { experimentId: experiment.experiment_id, instrument: instrument.instrument };
}

function MetricsCells({ metrics }: { readonly metrics: PredictionMetrics }) {
  if (metrics === null) {
    return (
      <td colSpan={3} data-label="Metrics">
        <Unavailable>Not evaluated</Unavailable>
      </td>
    );
  }
  if ('unevaluable' in metrics) {
    return (
      <td colSpan={3} data-label="Metrics">
        <Unavailable>{`Unevaluable: ${metrics.unevaluable.replaceAll('_', ' ')}`}</Unavailable>
      </td>
    );
  }
  return (
    <>
      <td className={historyStyles.number} data-label="Accuracy">
        <MetricValue metric={metrics.accuracy} />
      </td>
      <td className={historyStyles.number} data-label="Balanced accuracy">
        <MetricValue metric={metrics.balanced_accuracy} />
      </td>
      <td className={historyStyles.number} data-label="Rows">
        {metrics.rows}
      </td>
    </>
  );
}

// Classification metrics, kept apart from trading results on purpose.
function PredictionMetricsTable({ entry }: { readonly entry: ExperimentInstrument }) {
  return (
    <div className={styles.section}>
      <h3 className={styles.sectionTitle}>Prediction metrics · {entry.instrument}</h3>
      <p className={styles.note}>
        How often each model classified the next one-second midpoint move correctly. Accuracy is not profit: these
        numbers say nothing about trading results, which come only from an evaluation below.
      </p>
      <div className={historyStyles.scroll}>
        <table className={historyStyles.table}>
          <caption className="visually-hidden">Prediction metrics for {entry.instrument}</caption>
          <thead>
            <tr>
              <th scope="col">Window</th>
              <th scope="col">Model</th>
              <th scope="col" className={historyStyles.number}>
                Accuracy
              </th>
              <th scope="col" className={historyStyles.number}>
                Balanced accuracy
              </th>
              <th scope="col" className={historyStyles.number}>
                Rows
              </th>
            </tr>
          </thead>
          <tbody>
            {(['validation', 'test'] as const).flatMap((window) =>
              (['logistic', 'majority'] as const).map((model) => (
                <tr key={`${window}-${model}`}>
                  <td data-label="Window">{WINDOW_LABEL[window]}</td>
                  <td data-label="Model">{MODEL_LABEL[model]}</td>
                  <MetricsCells metrics={entry.prediction_metrics[window][model]} />
                </tr>
              )),
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}

function modelSummary(entry: ExperimentInstrument): { label: string; tone: 'positive' | 'caution' } {
  const logistic = entry.models.logistic;
  return logistic.status === 'fitted'
    ? { label: 'Logistic fitted', tone: 'positive' }
    : { label: `Logistic unevaluable: ${(logistic.reason ?? 'no reason given').replaceAll('_', ' ')}`, tone: 'caution' };
}

function ExperimentDetails({
  experiment,
  instrument,
  onInstrument,
}: {
  readonly experiment: Experiment;
  readonly instrument: string;
  readonly onInstrument: (instrument: string) => void;
}) {
  const legendId = useId();
  const selected = experiment.instruments.find((entry) => entry.instrument === instrument);
  const { windows, dataset } = experiment;
  return (
    <>
      <Facts
        facts={[
          { label: 'Experiment', value: <span className={styles.ids}>{experiment.experiment_id}</span> },
          { label: 'Dataset', value: `${dataset.provider} ${dataset.provider_dataset}` },
          { label: 'Stream', value: dataset.stream },
          { label: 'Events', value: dataset.events },
          { label: 'Label horizon (ns)', value: experiment.horizon_ns },
        ]}
      />
      <p className={styles.note}>
        Verified when read: {experiment.verified}.
      </p>
      <div className={styles.section}>
        <fieldset className={styles.choices} aria-describedby={legendId}>
          <legend id={legendId}>Instrument</legend>
          {experiment.instruments.map((entry) => {
            const summary = modelSummary(entry);
            return (
              <label key={entry.instrument} className={styles.choice}>
                <input
                  type="radio"
                  name="research-instrument"
                  value={entry.instrument}
                  checked={entry.instrument === instrument}
                  onChange={() => {
                    onInstrument(entry.instrument);
                  }}
                />
                <span className="numeric">{entry.instrument}</span>
                <Badge tone={summary.tone}>{summary.label}</Badge>
                <span className={styles.choiceDetail}>{entry.training.rows} training rows</span>
              </label>
            );
          })}
        </fieldset>
      </div>
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Windows</h3>
        <Facts
          facts={[
            { label: 'Training from', value: <MarketTime value={windows.training.start_at} stacked /> },
            { label: 'Training until', value: <MarketTime value={windows.training.end_at} stacked /> },
            {
              label: 'Training labels known by',
              value: <MarketTime value={windows.training.labels_available_through} stacked />,
            },
            { label: 'Validation', value: <MarketTime value={windows.validation.start_at} stacked /> },
            { label: 'Test', value: <MarketTime value={windows.test.start_at} stacked /> },
            { label: 'Data ends', value: <MarketTime value={dataset.receive_time.latest} stacked /> },
          ]}
        />
        <p className={styles.note}>
          Times are UTC receive times. Each window ends where the next begins; the test window runs to the end of the
          data. Every training label was known before validation began.
        </p>
      </div>
      {selected !== undefined && <PredictionMetricsTable entry={selected} />}
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Limitations</h3>
        <ul className={styles.list}>
          {experiment.limitations.map((text) => (
            <li key={text}>{text}</li>
          ))}
        </ul>
      </div>
    </>
  );
}

export function ModelCatalog({
  experiments,
  selection,
  onSelect,
}: {
  readonly experiments: UseInfiniteQueryResult<ExperimentPage>;
  readonly selection: Selection | null;
  readonly onSelect: (selection: Selection) => void;
}) {
  const choiceId = useId();
  if (experiments.status === 'pending') {
    return (
      <Panel title="Model">
        <p role="status">Loading experiments…</p>
      </Panel>
    );
  }
  // A failed older page keeps the loaded ones; only a first page that
  // failed replaces the list with the failure.
  if (experiments.data === undefined) {
    return (
      <Panel title="Model">
        <ResearchFailure
          error={experiments.error}
          title="The experiments could not be read"
          onRetry={() => void experiments.refetch()}
          retrying={experiments.isFetching}
        />
      </Panel>
    );
  }
  const page = experiments.data;
  const experiment = page.experiments.find((entry) => entry.experiment_id === selection?.experimentId);
  return (
    <Panel
      title="Model"
      meta={page.experiments.length === 1 ? '1 experiment' : `${String(page.experiments.length)} experiments`}
    >
      {page.invalid_entries.length > 0 && (
        <Alert tone="caution" title="Some experiments cannot be used">
          <ul className={styles.list}>
            {page.invalid_entries.map((entry) => (
              <li key={entry.experiment_id}>
                <Code>{shortId(entry.experiment_id)}</Code> {entry.reason}
              </li>
            ))}
          </ul>
        </Alert>
      )}
      {page.experiments.length === 0 ? (
        <div className={historyStyles.empty}>
          <p>No baseline experiments yet.</p>
          <p className={styles.note}>
            Build one from a local recording with <Code>python -m chronos_research demo</Code>; it appears here once
            published.
          </p>
        </div>
      ) : (
        <>
          {page.experiments.length > 1 && (
            <fieldset className={styles.choices} aria-describedby={choiceId}>
              <legend id={choiceId}>Experiment</legend>
              {page.experiments.map((entry) => (
                <label key={entry.experiment_id} className={styles.choice}>
                  <input
                    type="radio"
                    name="research-experiment"
                    checked={entry.experiment_id === selection?.experimentId}
                    onChange={() => {
                      onSelect({
                        experimentId: entry.experiment_id,
                        instrument: entry.instruments[0]?.instrument ?? '',
                      });
                    }}
                  />
                  <span className="mono">{shortId(entry.experiment_id)}</span>
                  <span className={styles.choiceDetail}>{entry.dataset.stream}</span>
                </label>
              ))}
            </fieldset>
          )}
          <ListFooter
            shown={page.experiments.length}
            noun="experiments"
            hasMore={experiments.hasNextPage}
            loading={experiments.isFetchingNextPage}
            failed={experiments.isFetchNextPageError}
            onLoadMore={() => void experiments.fetchNextPage()}
          />
          {experiment !== undefined && selection !== null && (
            <div className={page.experiments.length > 1 ? styles.section : undefined}>
              <ExperimentDetails
                experiment={experiment}
                instrument={selection.instrument}
                onInstrument={(instrument) => {
                  onSelect({ experimentId: experiment.experiment_id, instrument });
                }}
              />
            </div>
          )}
        </>
      )}
    </Panel>
  );
}
