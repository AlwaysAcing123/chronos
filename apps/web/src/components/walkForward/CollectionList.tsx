import type { CollectionMember } from '../../api/walkForwardSchemas';
import { nanosecondsAsSeconds } from '../../presentation/format';
import { shortId } from '../../presentation/research';
import { EVIDENCE } from '../../presentation/walkForward';
import { useCollections } from '../../state/walkForward';
import historyStyles from '../history/history.module.css';
import { ListFooter } from '../research/ListFooter';
import { ResearchFailure } from '../research/Research';
import styles from '../research/research.module.css';
import { Alert, Code } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { MarketTime } from '../ui/MarketTime';
import { Panel } from '../ui/Panel';

export const CREATE_COLLECTION_COMMAND =
  'python -m chronos_research create-collection --tools <build> --artifacts ../data --label-artifact <id> …';

function Session({ member }: { readonly member: CollectionMember }) {
  return 'unavailable' in member.session ? (
    <span className={styles.choiceDetail}>No session label</span>
  ) : (
    <>
      {member.session.label}
      <span className={styles.reasonCode}>Declared, not verified</span>
    </>
  );
}

// Every published collection of recordings, verified when listed, with
// each member's actual coverage.
export function CollectionList() {
  const collections = useCollections();
  return (
    <Panel title="Recording collections" meta="Verified when read">
      {collections.status === 'pending' ? (
        <p role="status">Loading collections…</p>
      ) : collections.data === undefined ? (
        <ResearchFailure
          error={collections.error}
          title="The collections could not be read"
          onRetry={() => void collections.refetch()}
          retrying={collections.isFetching}
        />
      ) : (
        <>
          {collections.data.invalid_entries.length > 0 && (
            <Alert tone="negative" title="Some collections do not verify and are not shown">
              <ul className={styles.list}>
                {collections.data.invalid_entries.map((entry) => (
                  <li key={entry.collection_id}>
                    <Code>{shortId(entry.collection_id)}</Code> {entry.reason}
                  </li>
                ))}
              </ul>
            </Alert>
          )}
          {collections.data.collections.length === 0 ? (
            <div className={historyStyles.empty}>
              <p>No collection of recordings is published yet.</p>
              <p className={styles.note}>
                Collect two or more independent recordings, each its own verified dataset, with{' '}
                <Code>{CREATE_COLLECTION_COMMAND}</Code>
              </p>
            </div>
          ) : (
            collections.data.collections.map((collection) => (
              <div key={collection.collection_id} className={styles.section}>
                <h3 className={styles.sectionTitle}>
                  Collection <span className={styles.ids}>{shortId(collection.collection_id)}</span>{' '}
                  <Badge tone={EVIDENCE[collection.evidence].tone}>{EVIDENCE[collection.evidence].label}</Badge>
                </h3>
                <div className={historyStyles.scroll}>
                  <table className={historyStyles.table}>
                    <caption className="visually-hidden">
                      Members of collection {collection.collection_id}, in time order
                    </caption>
                    <thead>
                      <tr>
                        <th scope="col">Recording</th>
                        <th scope="col">Session</th>
                        <th scope="col">Received</th>
                        <th scope="col">Rows per instrument</th>
                      </tr>
                    </thead>
                    <tbody>
                      {collection.members.map((member) => (
                        <tr key={member.dataset_id}>
                          <td data-label="Recording">
                            <span>
                              {member.stream}
                              <span className={styles.reasonCode}>{member.provenance.provider_dataset}</span>
                            </span>
                          </td>
                          <td data-label="Session">
                            <span>
                              <Session member={member} />
                            </span>
                          </td>
                          <td data-label="Received">
                            <span>
                              <MarketTime value={member.receive_time.earliest} stacked />
                              <span className={styles.reasonCode}>
                                {nanosecondsAsSeconds(member.coverage.duration_ns)} s, partial coverage
                              </span>
                            </span>
                          </td>
                          <td data-label="Rows per instrument" className={historyStyles.number}>
                            <span className={styles.stackedFigures}>
                              {Object.entries(member.instruments).map(([instrument, rows]) => (
                                <span key={instrument}>
                                  {instrument} {rows.feature_rows} rows, {rows.usable_labels} labelled
                                </span>
                              ))}
                            </span>
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              </div>
            ))
          )}
          <ListFooter
            shown={collections.data.collections.length}
            noun="collections"
            hasMore={collections.hasNextPage}
            loading={collections.isFetchingNextPage}
            failed={collections.isFetchNextPageError}
            onLoadMore={() => void collections.fetchNextPage()}
          />
          <p className={styles.note}>
            Chronos has no exchange calendar, so every recording is partial coverage of its session. A session label is
            its creator’s declaration. Synthetic recordings test the machinery, not a market.
          </p>
        </>
      )}
    </Panel>
  );
}
