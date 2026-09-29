// The order ticket's lifecycle, including reconciliation of submissions whose
// outcome is unknown (docs/paper-trading-api.md, "Reconciling an uncertain
// submission"). An uncertain order keeps its client order ID until a lookup
// proves what happened, so a retry can never record the order twice.
import { useQueryClient } from '@tanstack/react-query';
import { useCallback, useEffect, useReducer, useRef } from 'react';

import type { ApiFailure, FailureCategory } from '../api/errors';
import { categoryOf } from '../api/errors';
import type { ErrorBody, Order } from '../api/schemas';
import type { OrderRequest } from '../api/submission';
import { orderMatchesRequest } from '../api/submission';
import { queryKeys } from './queries';
import { useChronosServices } from './services';

// Why the ticket had to ask the server whether an order was recorded.
export type UncertainCause =
  | { readonly kind: 'no_definitive_response'; readonly failure: ApiFailure }
  | { readonly kind: 'id_already_used' };

export type TicketState =
  | { readonly step: 'editing' }
  | { readonly step: 'reviewing'; readonly request: OrderRequest }
  | { readonly step: 'submitting'; readonly request: OrderRequest }
  | {
      readonly step: 'recorded';
      readonly request: OrderRequest;
      readonly order: Order;
      // 'lookup' when the outcome was recovered after an uncertain response.
      readonly learnedFrom: 'response' | 'lookup';
    }
  | {
      readonly step: 'not_recorded';
      readonly request: OrderRequest;
      readonly error: ErrorBody;
      readonly category: FailureCategory;
      readonly transient: boolean;
    }
  | { readonly step: 'reconciling'; readonly request: OrderRequest; readonly cause: UncertainCause }
  // The lookup itself failed, so the outcome is still unknown.
  | {
      readonly step: 'unresolved';
      readonly request: OrderRequest;
      readonly cause: UncertainCause;
      readonly lookupFailure: FailureCategory;
    }
  // The lookup found no order with this ID: resubmitting it is safe.
  | { readonly step: 'confirmed_absent'; readonly request: OrderRequest; readonly cause: UncertainCause }
  // The ID belongs to a recorded order with different terms.
  | { readonly step: 'id_conflict'; readonly request: OrderRequest; readonly existing: Order };

type TicketAction =
  | { readonly type: 'review'; readonly request: OrderRequest }
  | { readonly type: 'edit' }
  | { readonly type: 'start_over' }
  | { readonly type: 'resolved'; readonly state: TicketState };

function ticketReducer(state: TicketState, action: TicketAction): TicketState {
  switch (action.type) {
    case 'review':
      return state.step === 'editing' ? { step: 'reviewing', request: action.request } : state;
    case 'edit':
      // An order whose outcome is unknown cannot be edited into a new one.
      return canEdit(state) ? { step: 'editing' } : state;
    case 'start_over':
      return canStartOver(state) ? { step: 'editing' } : state;
    case 'resolved':
      return action.state;
  }
}

type SendableState = Extract<TicketState, { step: 'reviewing' | 'confirmed_absent' | 'not_recorded' }>;

// States from which the same request may be sent (again).
function canSubmit(state: TicketState): state is SendableState {
  return (
    state.step === 'reviewing' ||
    state.step === 'confirmed_absent' ||
    (state.step === 'not_recorded' && state.transient)
  );
}

function canEdit(state: TicketState): boolean {
  return state.step === 'reviewing' || state.step === 'not_recorded';
}

// Leaving is allowed once the outcome is known, or proven unrecorded.
function canStartOver(state: TicketState): boolean {
  return (
    state.step === 'recorded' ||
    state.step === 'not_recorded' ||
    state.step === 'confirmed_absent' ||
    state.step === 'id_conflict'
  );
}

export function useOrderTicket(accountId: string) {
  const { client } = useChronosServices();
  const queryClient = useQueryClient();
  const [state, dispatch] = useReducer(ticketReducer, { step: 'editing' });
  const busy = useRef(false);
  const lookupController = useRef<AbortController | null>(null);
  const mounted = useRef(true);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      lookupController.current?.abort();
    };
  }, []);

  const resolve = useCallback((next: TicketState) => {
    if (mounted.current) {
      dispatch({ type: 'resolved', state: next });
    }
  }, []);

  const refreshAccount = useCallback(() => {
    void queryClient.invalidateQueries({ queryKey: queryKeys.account(accountId) });
  }, [accountId, queryClient]);

  const reconcile = useCallback(
    async (request: OrderRequest, cause: UncertainCause) => {
      resolve({ step: 'reconciling', request, cause });
      const controller = new AbortController();
      lookupController.current = controller;
      try {
        const lookup = await client.lookupOrder(accountId, request.client_order_id, controller.signal);
        if (lookup.kind === 'not_recorded') {
          resolve(
            cause.kind === 'id_already_used'
              // The server just said the ID exists; an empty lookup
              // contradicts that, so the outcome stays unknown.
              ? { step: 'unresolved', request, cause, lookupFailure: 'unexpected_response' }
              : { step: 'confirmed_absent', request, cause },
          );
        } else if (orderMatchesRequest(lookup.order, request)) {
          resolve({ step: 'recorded', request, order: lookup.order, learnedFrom: 'lookup' });
        } else {
          resolve({ step: 'id_conflict', request, existing: lookup.order });
        }
      } catch (error) {
        if (!controller.signal.aborted) {
          resolve({ step: 'unresolved', request, cause, lookupFailure: categoryOf(error) });
        }
      } finally {
        refreshAccount();
      }
    },
    [accountId, client, refreshAccount, resolve],
  );

  const send = useCallback(
    async (request: OrderRequest) => {
      if (busy.current) {
        return;
      }
      busy.current = true;
      try {
        resolve({ step: 'submitting', request });
        const response = await client.submitOrder(accountId, request);
        switch (response.kind) {
          case 'recorded':
            resolve({ step: 'recorded', request, order: response.order, learnedFrom: 'response' });
            refreshAccount();
            break;
          case 'not_recorded':
            resolve({
              step: 'not_recorded',
              request,
              error: response.error,
              category: response.category,
              transient: response.transient,
            });
            break;
          case 'id_already_used':
            await reconcile(request, { kind: 'id_already_used' });
            break;
          case 'uncertain':
            await reconcile(request, { kind: 'no_definitive_response', failure: response.failure });
            break;
        }
      } finally {
        busy.current = false;
      }
    },
    [accountId, client, reconcile, refreshAccount, resolve],
  );

  const submit = useCallback(() => {
    if (canSubmit(state)) {
      void send(state.request);
    }
  }, [send, state]);

  const checkAgain = useCallback(() => {
    if (state.step === 'unresolved' && !busy.current) {
      busy.current = true;
      void reconcile(state.request, state.cause).finally(() => {
        busy.current = false;
      });
    }
  }, [reconcile, state]);

  const review = useCallback((request: OrderRequest) => {
    dispatch({ type: 'review', request });
  }, []);
  const edit = useCallback(() => {
    dispatch({ type: 'edit' });
  }, []);
  const startOver = useCallback(() => {
    dispatch({ type: 'start_over' });
  }, []);

  return { state, review, edit, submit, checkAgain, startOver };
}
