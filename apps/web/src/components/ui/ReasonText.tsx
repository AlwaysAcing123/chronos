import type { ErrorBody } from '../../api/schemas';
import { reasonLabel } from '../../presentation/vocabulary';
import { Code } from './Alert';

// A server error's stable reason in words with its code, or its message
// when it has no reason.
export function ReasonText({ error }: { readonly error: Pick<ErrorBody, 'message' | 'reason'> }) {
  return error.reason === null ? (
    <>{error.message}</>
  ) : (
    <>
      {reasonLabel(error.reason)} <Code>{error.reason}</Code>
    </>
  );
}
