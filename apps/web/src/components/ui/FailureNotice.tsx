import { ChronosApiError, categoryOf } from '../../api/errors';
import { FAILURE, reasonLabel } from '../../presentation/vocabulary';
import { Alert, Code } from './Alert';
import { Button } from './Button';

// Explains a failed read and offers a retry. Server messages are shown as
// given; unexpected responses show only their contract mismatch.
export function FailureNotice({
  error,
  onRetry,
  retrying = false,
}: {
  readonly error: unknown;
  readonly onRetry?: () => void;
  readonly retrying?: boolean;
}) {
  const category = categoryOf(error);
  const description = FAILURE[category];
  const failure = error instanceof ChronosApiError ? error.failure : null;
  return (
    <Alert
      tone={category === 'network' || category === 'persistence_unavailable' ? 'caution' : 'negative'}
      title={description.title}
      actions={
        onRetry && (
          <Button onClick={onRetry} disabled={retrying}>
            {retrying ? 'Retrying…' : 'Retry'}
          </Button>
        )
      }
    >
      <p>{description.detail}</p>
      {failure?.kind === 'api_error' && failure.error.reason !== null && (
        <p>
          {reasonLabel(failure.error.reason)} <Code>{failure.error.reason}</Code>
        </p>
      )}
      {failure?.kind === 'unexpected_response' && <p>{failure.detail}</p>}
    </Alert>
  );
}
