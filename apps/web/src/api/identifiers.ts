// Account IDs, client order IDs, source IDs, and instruments share one server
// rule: 1 to 64 characters from [A-Za-z0-9._-], starting with a letter or digit.
export const IDENTIFIER_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;

export function isValidIdentifier(text: string): boolean {
  return IDENTIFIER_PATTERN.test(text);
}

/**
 * A fresh client order ID. It is generated once per reviewed order and then
 * reused for every retry of that order, so the server's per-account
 * uniqueness guarantee prevents a duplicate recording.
 */
export function newClientOrderId(now: number = Date.now()): string {
  return `web-${uniqueSuffix(now)}`;
}

/**
 * A fresh cancel request ID, generated once per cancellation of an order and
 * reused for every retry of it: the server accepts one ID for at most one
 * cancellation, so a retry after an uncertain result cannot cancel twice.
 */
export function newCancelRequestId(now: number = Date.now()): string {
  return `cancel-${uniqueSuffix(now)}`;
}

// Millisecond time keeps IDs readable and ordered; ten random base-36
// characters (about 50 bits) make a collision within an account negligible.
function uniqueSuffix(now: number): string {
  const random = Array.from(crypto.getRandomValues(new Uint32Array(2)), (word) =>
    word.toString(36).padStart(7, '0'),
  )
    .join('')
    .slice(0, 10);
  return `${now.toString(36)}-${random}`;
}
