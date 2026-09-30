// Cancellation of working orders, including reconciliation of requests whose
// outcome is unknown (docs/paper-trading-api.md, the cancel endpoint). Each
// order keeps one cancel request ID from its first attempt
// until the cancellation is settled, so every retry is the same request and
// the server cancels at most once.
import { useQueryClient } from '@tanstack/react-query';
import { useCallback, useEffect, useRef, useState } from 'react';

import type { ApiFailure, FailureCategory } from '../api/errors';
import { categoryOf } from '../api/errors';
import { newCancelRequestId } from '../api/identifiers';
import type { ErrorBody, Order } from '../api/schemas';
import { isWorking } from '../api/schemas';
import { queryKeys } from './queries';
import { useChronosServices } from './services';

export type CancelState =
  | { readonly step: 'cancelling'; readonly requestId: string }
  // The response was inconclusive; reading the order to learn what happened.
  | { readonly step: 'reconciling'; readonly requestId: string; readonly failure: ApiFailure }
  // The order still works and this request has not cancelled it, so
  // sending the same request again is safe.
  | { readonly step: 'not_cancelled'; readonly requestId: string; readonly failure: ApiFailure }
  // The lookup failed too: the cancellation may or may not have happened.
  | {
      readonly step: 'unresolved';
      readonly requestId: string;
      readonly failure: ApiFailure;
      readonly lookupFailure: FailureCategory;
    }
  | {
      readonly step: 'cancelled';
      readonly requestId: string;
      readonly order: Order;
      // How the outcome was learned: this response, a retry's response, or a
      // lookup after an inconclusive one.
      readonly learnedFrom: 'response' | 'retry' | 'lookup';
    }
  // It filled, was cancelled by another request, or never worked.
  | { readonly step: 'not_cancellable'; readonly requestId: string; readonly order: Order }
  | {
      readonly step: 'refused';
      readonly requestId: string;
      readonly error: ErrorBody;
      readonly category: FailureCategory;
    };

// States in which the same request may be sent again.
export function canRetryCancel(state: CancelState): boolean {
  return state.step === 'not_cancelled' || state.step === 'unresolved';
}

// States whose outcome is known, which the user may dismiss.
export function isSettled(state: CancelState): boolean {
  return state.step === 'cancelled' || state.step === 'not_cancellable' || state.step === 'refused';
}

export function useOrderCancellation(accountId: string) {
  const { client } = useChronosServices();
  const queryClient = useQueryClient();
  const [states, setStates] = useState<ReadonlyMap<string, CancelState>>(new Map());
  // The latest state per order, for callbacks that run after a render.
  const latest = useRef(new Map<string, CancelState>());
  const busy = useRef(new Set<string>());
  const mounted = useRef(true);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  const put = useCallback((clientOrderId: string, state: CancelState | null) => {
    if (state === null) {
      latest.current.delete(clientOrderId);
    } else {
      latest.current.set(clientOrderId, state);
    }
    if (mounted.current) {
      setStates(new Map(latest.current));
    }
  }, []);

  const refreshAccount = useCallback(() => {
    void queryClient.invalidateQueries({ queryKey: queryKeys.account(accountId) });
  }, [accountId, queryClient]);

  const reconcile = useCallback(
    async (clientOrderId: string, requestId: string, failure: ApiFailure) => {
      put(clientOrderId, { step: 'reconciling', requestId, failure });
      try {
        const lookup = await client.lookupOrder(accountId, clientOrderId);
        if (lookup.kind === 'not_recorded') {
          // A working order cannot disappear, so the answer is not credible.
          put(clientOrderId, { step: 'unresolved', requestId, failure, lookupFailure: 'unexpected_response' });
        } else if (lookup.order.cancel_request_id === requestId) {
          put(clientOrderId, { step: 'cancelled', requestId, order: lookup.order, learnedFrom: 'lookup' });
        } else if (isWorking(lookup.order.status)) {
          put(clientOrderId, { step: 'not_cancelled', requestId, failure });
        } else {
          put(clientOrderId, { step: 'not_cancellable', requestId, order: lookup.order });
        }
      } catch (error) {
        put(clientOrderId, { step: 'unresolved', requestId, failure, lookupFailure: categoryOf(error) });
      }
    },
    [accountId, client, put],
  );

  const send = useCallback(
    async (clientOrderId: string, requestId: string, retry: boolean) => {
      put(clientOrderId, { step: 'cancelling', requestId });
      const response = await client.cancelOrder(accountId, clientOrderId, requestId);
      switch (response.kind) {
        case 'cancelled':
          put(clientOrderId, {
            step: 'cancelled',
            requestId,
            order: response.order,
            learnedFrom: retry || response.alreadyCancelled ? 'retry' : 'response',
          });
          break;
        case 'not_cancellable':
          put(clientOrderId, { step: 'not_cancellable', requestId, order: response.order });
          break;
        case 'refused':
          put(clientOrderId, { step: 'refused', requestId, error: response.error, category: response.category });
          break;
        case 'uncertain':
          await reconcile(clientOrderId, requestId, response.failure);
          break;
      }
    },
    [accountId, client, put, reconcile],
  );

  const run = useCallback(
    (clientOrderId: string, action: () => Promise<void>) => {
      if (busy.current.has(clientOrderId)) {
        return;
      }
      busy.current.add(clientOrderId);
      void action().finally(() => {
        busy.current.delete(clientOrderId);
        refreshAccount();
      });
    },
    [refreshAccount],
  );

  // Starts a cancellation, or retries the unsettled one with its own ID.
  const cancel = useCallback(
    (clientOrderId: string) => {
      const current = latest.current.get(clientOrderId);
      if (current !== undefined && !isSettled(current) && !canRetryCancel(current)) {
        return;
      }
      const retry = current !== undefined && canRetryCancel(current);
      const requestId = retry ? current.requestId : newCancelRequestId();
      run(clientOrderId, () => send(clientOrderId, requestId, retry));
    },
    [run, send],
  );

  const checkAgain = useCallback(
    (clientOrderId: string) => {
      const current = latest.current.get(clientOrderId);
      if (current?.step === 'unresolved') {
        run(clientOrderId, () => reconcile(clientOrderId, current.requestId, current.failure));
      }
    },
    [reconcile, run],
  );

  const dismiss = useCallback(
    (clientOrderId: string) => {
      const current = latest.current.get(clientOrderId);
      if (current !== undefined && isSettled(current)) {
        put(clientOrderId, null);
      }
    },
    [put],
  );

  return { states, cancel, checkAgain, dismiss };
}
