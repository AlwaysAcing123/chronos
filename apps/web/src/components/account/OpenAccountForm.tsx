import { useQueryClient } from '@tanstack/react-query';
import type { SubmitEvent } from 'react';
import { useState } from 'react';

import { categoryOf } from '../../api/errors';
import { isValidIdentifier } from '../../api/identifiers';
import { FAILURE } from '../../presentation/vocabulary';
import { queryKeys } from '../../state/queries';
import { useChronosServices } from '../../state/services';
import { TextField } from '../form/Field';
import formStyles from '../form/form.module.css';
import { IDENTIFIER_RULE } from '../form/messages';
import { Alert } from '../ui/Alert';
import { Button } from '../ui/Button';
import { Panel } from '../ui/Panel';
import styles from './account.module.css';

export function OpenAccountForm({
  recentAccountIds,
  onOpen,
  onForget,
}: {
  readonly recentAccountIds: readonly string[];
  readonly onOpen: (accountId: string) => void;
  readonly onForget: (accountId: string) => void;
}) {
  const { client } = useChronosServices();
  const queryClient = useQueryClient();
  const [accountId, setAccountId] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [failure, setFailure] = useState<ReturnType<typeof categoryOf> | null>(null);
  const [pending, setPending] = useState(false);

  async function submit(event: SubmitEvent<HTMLFormElement>) {
    event.preventDefault();
    setFailure(null);
    if (!isValidIdentifier(accountId)) {
      setError(IDENTIFIER_RULE);
      return;
    }
    setError(null);
    setPending(true);
    try {
      // Opening checks existence first, so a mistyped ID never becomes the selection.
      await queryClient.query({
        queryKey: queryKeys.accountResource(accountId),
        queryFn: ({ signal }) => client.getAccount(accountId, signal),
      });
      onOpen(accountId);
    } catch (caught) {
      const category = categoryOf(caught);
      if (category === 'account_not_found') {
        setError('No paper account with this ID exists on this server.');
      } else {
        setFailure(category);
      }
    } finally {
      setPending(false);
    }
  }

  return (
    <Panel title="Open an existing account">
      <form
        className={formStyles.form}
        onSubmit={(event) => {
          void submit(event);
        }}
        noValidate
      >
        <TextField
          label="Account ID"
          name="open_account_id"
          value={accountId}
          onChange={(event) => {
            setAccountId(event.target.value);
          }}
          autoComplete="off"
          spellCheck={false}
          error={error}
          disabled={pending}
        />
        {failure !== null && (
          <Alert tone="caution" title={FAILURE[failure].title} live>
            {FAILURE[failure].detail}
          </Alert>
        )}
        <div className={formStyles.actions}>
          <Button type="submit" disabled={pending}>
            {pending ? 'Opening…' : 'Open account'}
          </Button>
        </div>
      </form>
      {recentAccountIds.length > 0 && (
        <nav aria-labelledby="recent-accounts-heading" className={styles.recentSection}>
          <h3 id="recent-accounts-heading" className={styles.recentHeading}>
            Recently opened in this browser
          </h3>
          <ul className={styles.recent}>
            {recentAccountIds.map((id) => (
              <li key={id} className={styles.recentItem}>
                <span className={styles.recentId}>{id}</span>
                <span>
                  <Button
                    variant="quiet"
                    onClick={() => {
                      onOpen(id);
                    }}
                    aria-label={`Open ${id}`}
                  >
                    Open
                  </Button>
                  <Button
                    variant="quiet"
                    onClick={() => {
                      onForget(id);
                    }}
                    aria-label={`Remove ${id} from recent accounts`}
                  >
                    Remove
                  </Button>
                </span>
              </li>
            ))}
          </ul>
        </nav>
      )}
    </Panel>
  );
}
