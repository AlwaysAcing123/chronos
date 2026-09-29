import { useMutation, useQueryClient } from '@tanstack/react-query';
import type { SubmitEvent } from 'react';
import { useState } from 'react';

import { FRACTION_DIGITS, validatePositiveDecimal } from '../../api/decimal';
import { ChronosApiError, categoryOf } from '../../api/errors';
import { isValidIdentifier } from '../../api/identifiers';
import { FAILURE } from '../../presentation/vocabulary';
import { queryKeys, useHealth } from '../../state/queries';
import { useChronosServices } from '../../state/services';
import { TextField } from '../form/Field';
import formStyles from '../form/form.module.css';
import { decimalProblemMessage, IDENTIFIER_RULE } from '../form/messages';
import { Alert } from '../ui/Alert';
import { Button } from '../ui/Button';
import { Panel } from '../ui/Panel';

interface FieldErrors {
  readonly account_id?: string;
  readonly starting_cash?: string;
}

// Field errors the server reports for account creation, by request field.
// A taken ID is explained by its own notice, which offers to open it.
function serverFieldErrors(error: unknown): FieldErrors {
  if (
    !(error instanceof ChronosApiError) ||
    error.failure.kind !== 'api_error' ||
    error.failure.error.code === 'account_already_exists'
  ) {
    return {};
  }
  const { field, message } = error.failure.error;
  return field === 'account_id' || field === 'starting_cash' ? { [field]: message } : {};
}

export function CreateAccountForm({ onOpen }: { readonly onOpen: (accountId: string) => void }) {
  const { client } = useChronosServices();
  const queryClient = useQueryClient();
  const health = useHealth();
  const [accountId, setAccountId] = useState('');
  const [startingCash, setStartingCash] = useState('100000');
  const [errors, setErrors] = useState<FieldErrors>({});

  const creation = useMutation({
    mutationFn: (request: Parameters<typeof client.createAccount>[0]) => client.createAccount(request),
    onSuccess: (account) => {
      queryClient.setQueryData(queryKeys.accountResource(account.account_id), account);
      onOpen(account.account_id);
    },
    onError: (error) => {
      setErrors(serverFieldErrors(error));
    },
  });

  const instruments = health.status === 'success' ? health.data.tradeable_instruments : [];

  function submit(event: SubmitEvent<HTMLFormElement>) {
    event.preventDefault();
    const cash = validatePositiveDecimal(startingCash, 'money');
    const next: FieldErrors = {
      ...(isValidIdentifier(accountId) ? {} : { account_id: IDENTIFIER_RULE }),
      ...(cash.ok ? {} : { starting_cash: decimalProblemMessage(cash.problem, FRACTION_DIGITS.money) }),
    };
    setErrors(next);
    if (Object.keys(next).length === 0) {
      creation.mutate({ account_id: accountId, starting_cash: startingCash });
    }
  }

  const failure = creation.isError ? categoryOf(creation.error) : null;
  const pending = creation.isPending;

  return (
    <Panel title="Create a paper account">
      <form className={formStyles.form} onSubmit={submit} noValidate>
        <TextField
          label="Account ID"
          name="account_id"
          value={accountId}
          onChange={(event) => {
            setAccountId(event.target.value);
          }}
          autoComplete="off"
          spellCheck={false}
          placeholder="e.g. research-1"
          hint={IDENTIFIER_RULE}
          error={errors.account_id}
          disabled={pending}
        />
        {/* Every account may trade every instrument the server offers from one cash balance. */}
        <p className={formStyles.hint} aria-live="polite">
          {instruments.length > 0
            ? `Trades ${instruments.join(', ')} from one shared cash balance.`
            : health.status === 'error'
              ? 'The instruments this server trades are unavailable: the API is unreachable.'
              : 'Loading the instruments this server trades…'}
        </p>
        <TextField
          label="Starting cash (simulated)"
          name="starting_cash"
          value={startingCash}
          onChange={(event) => {
            setStartingCash(event.target.value);
          }}
          inputMode="decimal"
          autoComplete="off"
          spellCheck={false}
          hint="Sent exactly as typed, up to 8 decimal places."
          error={errors.starting_cash}
          disabled={pending}
        />
        {failure === 'account_already_exists' && (
          <Alert
            tone="caution"
            title={FAILURE.account_already_exists.title}
            live
            actions={
              <Button
                onClick={() => {
                  onOpen(accountId);
                }}
              >
                Open {accountId}
              </Button>
            }
          >
            {FAILURE.account_already_exists.detail}
          </Alert>
        )}
        {(failure === 'persistence_unavailable' || failure === 'internal_error' || failure === 'network' ||
          failure === 'unexpected_response') && (
          <Alert tone="caution" title={FAILURE[failure].title} live>
            The account may or may not have been created. Submitting again with the same ID is safe: it either
            creates the account or reports that it already exists.
          </Alert>
        )}
        <div className={formStyles.actions}>
          <Button type="submit" variant="primary" disabled={pending}>
            {pending ? 'Creating…' : 'Create paper account'}
          </Button>
        </div>
      </form>
    </Panel>
  );
}
