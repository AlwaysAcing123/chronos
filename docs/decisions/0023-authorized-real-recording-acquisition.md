# ADR 0023: Authorized acquisition of real recordings for walk-forward studies

- Status: Proposed
- Date: 2026-10-09

## Context

Walk-forward studies (ADR 0022) need several independent real recordings,
and the project holds one. Each new recording comes from a paid provider:
Databento bills streaming on delivered bytes, bills a repeated request again,
and gives estimates that are not charges. `chronos_databento_historical`
already quotes, refuses a fresh estimate above `--max-cost-usd`, downloads
write-once, and validates every record, but one invocation knows nothing of
any other: run twice, or after a crash, it can pay twice.

Constraints:

- **No spending without a person.** Holding a key, a working quote, or a
  command with a cost limit is not authorization.
- **A charge may have happened whenever an attempt did not clearly succeed.**
  A timeout, a crash, a killed parent, or output that does not verify is not
  evidence that no request reached the provider.
- **Exact money.** Approvals are decimal amounts; the C++ limit is a double.
- **The rest stays offline.** Preparation, studies, the research service,
  and the browser must be unable to trigger a download.

## Decision

- **A request plan** (`chronos.recording_request_plan/1`) binds the provider
  query, symbols, each recording's stream, `ts_recv` window, declared session,
  and destination or existing file, and the frozen evaluation settings. Its
  ID is the SHA-256 of its canonical JSON.
- **Separate commands**: `plan-recordings` and `prepare-recordings` are
  offline; `quote-recordings` is metadata only; `authorize-acquisition`
  records the human decision; `acquire-recordings` alone can spend;
  `recover-acquisition` is an explicit operator action.
- **Authorization** (`chronos.acquisition_authorization/1`): the typed plan
  ID, the canonical state root, an allowance and positive per-recording caps
  in whole cents (caps ≤ allowance ≤ 100.00), published read-only. No default
  budget; uncapped recordings are never downloaded; one authorization per
  plan per root.
- **Ledger**: append-only, fsynced, canonical JSON lines. Each attempt
  reserves its full cap before the provider child starts; reservations never
  exceed the allowance. Replay holds every reservation to the authorization
  and plan: a capped recording of the plan, at its own destination, for
  exactly its cap, at most once, and every ending to one Chronos writes.
  Anything unexpected, or a ledger missing for a published authorization,
  fails closed.
- **A trusted, private state root**: its namespaces and every destination
  directory must be private real directories, and its state files regular
  files opened without following links, checked under the lock before any
  reservation (refused as `unsafe_path`). A concurrent hostile writer inside
  the root is out of scope: the C++ tool takes a path, not a descriptor.
- **No retry, no refund.** An attempt that is not a verified publication
  blocks its destination for every authorization in the root. Retrying needs
  a new destination, plan, and authorization, after reconciliation with the
  provider's billing.
- **One root lock, inherited by the child.** An exclusive non-blocking
  `flock`, passed to the provider child, so a child that outlives its parent
  keeps the root locked. An attempt never seen to finish blocks the root until
  `recover-acquisition --confirm-no-provider-process`, which needs the lock.
  A dead PID clears nothing.
- **Bounded children**: fixed argv, a minimal environment, their own process
  group, a timeout, bounded stdout and stderr; the key's value is removed from
  everything persisted or printed.
- **Verified reuse**: a recording counts only after its size and SHA-256
  match its manifest, the manifest matches the plan, `chronos_databento_replay`
  recomputes the manifest, and its receipt agrees with plan, ledger, and file.
- **The cap at the C++ boundary** is the largest double at most the
  authorized cents, written as its shortest decimal text.
- **The generic acquisition seam** refuses a non-finite or negative estimate
  from any provider before downloading (`NaN > limit` is false).
- **Dataset provenance** names the `ts_recv` window.

## Consequences

- A person decides every amount; the ledger is the record of every paid
  attempt, and a possibly charged one is never repeated by Chronos.
- After an ambiguous attempt, an operator reconciles by hand; Chronos does
  not try to infer whether a charge occurred.
- Datasets normalized from the same recording before and after the
  provenance fix differ in their `semantics` text and IDs; their events are
  identical and earlier datasets still verify.

## Alternatives rejected

- **Automatic retry after a failure**: a failed attempt may have been
  billed; retrying converts uncertainty into a possible second charge.
- **Refunding a reservation when the tool reports a pre-request error**: the
  tool's code path says so, but the invariant is simpler and safer when every
  reservation stays.
- **Clearing a lock whose parent PID is dead**: the provider child may still
  be downloading.
- **A Python provider SDK or a second adapter**: the C++ tool already holds
  the provider boundary and its validation.
- **A browser or service acquisition endpoint**: spending must stay a
  deliberate local command.

## Limitations

- The allowance bounds approved estimates, not the provider's invoice; actual
  cost is unknown unless Databento's billing says otherwise.
- Deleting a state root deletes its memory of attempts; that is a human act.
- `flock` and `fsync` are relied on only on local POSIX filesystems.
- Path checks assume a private root: they do not stop another process with
  write access inside it from swapping a directory after the last check.
- A torn last ledger line (a crash mid-write) fails closed and needs manual
  repair.
