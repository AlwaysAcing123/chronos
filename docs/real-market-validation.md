# Real-market walk-forward validation

Chronos can evaluate its models on several independent real recordings, but
only through a workflow that keeps spending separate from everything else.
One command can cost money, and only under an authorization a person
recorded for one exact plan. Everything before and after it is offline.

```text
plan-recordings        offline              validate the request plan, print its ID
quote-recordings       metadata only        estimates per recording; never downloads
authorize-acquisition  a human decision     allowance and caps for one exact plan
acquire-recordings     CAN SPEND MONEY      each authorized recording at most once
recover-acquisition    operator action      close an attempt never seen to finish
prepare-recordings     offline              verify, prepare, frozen studies, audits
```

Nothing in the browser or the research service can acquire data: the
service has no acquisition job and the Research view has no such control.

## The request plan

A `chronos.recording_request_plan/1` (see
`python/chronos_research/recording_plan.py`) fixes everything before any
quote or result:

- the provider binding, compared exactly: Databento `EQUS.MINI`, `mbp-1`,
  `raw_symbol` input, `instrument_id` output;
- sorted symbols;
- each recording's unique stream ID, UTC window (`YYYY-MM-DDTHH:MM:00Z`,
  start inclusive, end exclusive, 1 to 60 minutes, windows strictly disjoint
  and in start order), a declared session label, and either a `destination`
  (a relative `….dbn.zst` path under the state root's `recordings/`) or an
  `existing` recording (absolute path, SHA-256, size);
- the evaluation: instruments, label horizon, minimum training recordings,
  the strategy's eight options, and one to four fee/latency scenarios, held to
  the walk-forward planner's own rules and limits.

Every value is a string; unknown keys, duplicate keys, numbers, overlapping
windows, unsafe paths, and unsupported settings are refused. The plan ID is
the SHA-256 of the canonical plan, so an approval names exactly one plan.

**Time semantics.** Databento filters an MBP-1 request on `ts_recv`, the
record's index timestamp, not on `ts_event` (the official client documents
start and end as filtering on `ts_recv` when the schema has it; the pinned
C++ SDK's `Mbp1Msg::IndexTs()` returns `ts_recv`). Datasets normalized since
this checkpoint record "Databento query window by receive timestamp
(ts_recv)". Datasets published earlier say "by event timestamp"; their events
are unchanged and the provider applied the same `ts_recv` window, but a
recording normalized again now gets a dataset whose manifest text, and so ID,
differ ([normalized-market-datasets.md](normalized-market-datasets.md)).

**What the data is.** `EQUS.MINI` is Databento's derived composite mini-BBO:
not SIP, not the NBBO, and not full depth (ADR 0001). A pilot of a few
ten-minute windows is partial coverage of a few sessions, and a declared
session label is not proof of a complete session. Synthetic recordings from
the test generator are labelled synthetic everywhere and are never evidence
about a market.

## 1. Plan (offline)

```sh
cd python
.venv/bin/python -m chronos_research plan-recordings --plan ../my-plan.json
```

It prints the canonical plan and its `plan_id`, and spends nothing.

## 2. Quote (metadata only)

```sh
read -rs DATABENTO_API_KEY && export DATABENTO_API_KEY   # in this shell only; never on a command line
.venv/bin/python -m chronos_research quote-recordings --plan ../my-plan.json --tools ../build-databento
```

For each new recording it runs `chronos_databento_historical quote`, which
asks Databento's metadata endpoints for the estimated records, billable
bytes, and cost; Databento documents metadata requests as free. It verifies
each `existing` recording offline and lists it apart. The output separates:

- `items[].estimate`: the provider SDK's estimate, read exactly as decimal
  text (its own precision is whatever the SDK's double printed);
- `items[].proposed_cap_usd`: 1.25 times the estimate, up to the next cent;
- `proposed_allowance_usd`: the sum of the proposed caps;
- `existing_inputs`: recordings that cost nothing more;
- `actual_cost`: unknown. Streaming is billed on bytes delivered, a repeated
  request is billed again, and an estimate is not a charge.

A recording whose estimated record count is zero is not available for that
window: change the plan before authorizing anything.

## 3. Decide the budget (a person)

Read the quote. If you accept it, authorize the exact plan:

```sh
mkdir -m 700 ../data/real-acquisition
.venv/bin/python -m chronos_research authorize-acquisition --plan ../my-plan.json \
  --plan-id <plan_id from the quote> --state-root ../data/real-acquisition \
  --allowance-usd 1.20 --cap <stream-id>=0.30 --cap <stream-id>=0.30 …
```

The plan ID must be typed; the state root must already exist. Amounts are
whole cents, `D.CC`; the caps may not add up to more than the allowance, and
the allowance is at most 100.00 as a typo guard. **There is no default
budget**, and a recording without a cap is never downloaded. A state root
holds one authorization per plan; changing the plan, the root, or any amount
needs a new authorization.

## 4. Acquire (the only step that can cost money)

```sh
.venv/bin/python -m chronos_research acquire-recordings --state-root ../data/real-acquisition \
  --authorization <authorization_id> --tools ../build-databento
```

For each capped recording that is not yet verified, in plan order:

1. the attempt and its full cap are written to the ledger and fsynced
   **before** the provider child starts;
2. `chronos_databento_historical download` quotes again and stops if the fresh
   estimate exceeds the cap, then downloads, validates every record, and
   publishes the recording and its manifest write-once;
3. Chronos verifies the result itself (size, SHA-256, manifest against the
   plan, the manifest recomputed by `chronos_databento_replay`) and writes a
   receipt (`receipts/<authorization>/<stream>.json`) with the attempt, the
   fresh quote, and `actual_cost: unknown`.

The run stops at the first attempt that does not publish. The cap passed to
the C++ tool is the largest double at most the authorized cents, so it can be
a unit in the last place below the authorization, never above it.

### Running it again, and what never happens again

Running `acquire-recordings` again reuses every verified recording and
attempts only recordings that were never attempted. An attempt that did not
end in a verified recording (the tool refused or failed, timed out, crashed,
or reported success with output that does not verify) **may already have
been charged**. Chronos never retries it, never refunds its reservation, and
reports its destination as `needs_reconciliation` for every authorization in
the state root. If files it left at the destination verify on their own,
they are used, not downloaded again.

### After an ambiguous download

1. Do not delete the ledger, the receipts, or the state root: they are the
   only record of what may have been charged.
2. If `acquire-recordings` says `attempt_in_progress`, a provider child may
   still be running. Check (`pgrep -fl chronos_databento_historical`); while
   it runs, every command on that root answers `busy`, because the child
   holds the root's lock. When none is running, close the attempt:
   ```sh
   .venv/bin/python -m chronos_research recover-acquisition --state-root ../data/real-acquisition \
     --authorization <authorization_id> --attempt <n> --confirm-no-provider-process
   ```
   This records the attempt as ambiguous. It refunds and retries nothing.
3. Reconcile with Databento's account usage and billing.
4. To try that window again, write a new plan with a new destination, quote
   it, and authorize it as a new decision.

The state root needs a local POSIX filesystem (APFS, ext4); `flock` and
`fsync` are not dependable on network filesystems.

**The state root is trusted and private.** Create it yourself (`mkdir -m 700`),
owned by you and writable by no one else. Under its lock, before anything is
reserved, Chronos refuses (`unsafe_path`) a root, namespace (`authorizations/`,
`ledgers/`, `receipts/`, `recordings/`), or destination directory that is a
symbolic link (dangling or not), not a directory, not yours, or writable by
others, and a lock, ledger, receipt, recording, or manifest that is a link or
not a regular file. It creates missing destination directories one level at
a time inside the root and checks the whole path again just before it
reserves and launches. It does not defend against another process that can
write inside the root and swaps a directory between that check and the
provider tool's own open: the C++ tool takes a path, not a directory
descriptor, so a shared or hostile-writable root is not supported.

**The ledger is checked against what was approved.** Every reservation must be
a capped recording of the authorized plan, at that recording's own
destination, for exactly its cap, and at most once; every ending must be one
Chronos writes. A ledger that is merely well-formed but disagrees is refused
(`state_corrupt`) by acquisition, recovery, and preparation alike, before any
provider tool starts. An error code the tool never writes is recorded as
`unrecognized_error_code` (ambiguous), and its text is never echoed.

## 5. Prepare and evaluate (offline)

```sh
.venv/bin/python -m chronos_research prepare-recordings --plan ../my-plan.json \
  --tools ../build-databento --artifacts ../data/real-pilot --state-root ../data/real-acquisition \
  --markdown ../real-pilot-report.md
```

It verifies every recording again (and each receipt against the plan, the
ledger, and the file), normalizes it, extracts features and labels with the
plan's horizon, builds one collection with the declared session labels, then
for each instrument publishes the walk-forward plan the request plan froze
(checked equal to it), runs the study, and runs the independent auditor
(`python -I -S tools/walk_forward_audit/audit.py`). Recordings not acquired
are listed as pending with their reasons; a study the planner refuses, such
as one with fewer than two recordings, is reported with its refusal. The
report links request → raw SHA-256 and manifest → dataset → features →
labels → collection → plan → study → audit, and copies every figure from the
study with each unavailable value's reason.

## 6. Inspect in the browser

```sh
.venv/bin/python -m chronos_research serve --tools ../build-databento --artifacts ../data/real-pilot \
  --state ../data/real-pilot/research --port 18190
cd ../apps/web && CHRONOS_RESEARCH_ORIGIN=http://127.0.0.1:18190 npm run dev
```

Open **Research → Walk-forward evaluation**: the collection shows its
evidence (`observed`), coverage, and declared sessions; each study shows its
folds, scenarios, verification, and unavailable values with reasons.

## No-spend path (CI and any machine)

Everything except steps 2 and 4 runs without a key or a provider:

- `python -m pytest tests/test_recording_plan.py tests/test_acquisition.py
  tests/test_recording_pilot.py` covers plan validation, exact money,
  authorization, the ledger, concurrency, crashes, timeouts, reuse, and
  secrets against fake provider tools, and the whole preparation, study, and
  audit chain on deterministic synthetic sessions.
- With the licensed recording and a Databento build present,
  `CHRONOS_REAL_RECORDING=<recording> CHRONOS_DATABENTO_TOOLS=<build dir>`
  also verifies it offline and checks that one session is refused.
- `prepare-recordings` on a plan whose only available recording is the
  existing one verifies and prepares it and reports the rest as pending.
