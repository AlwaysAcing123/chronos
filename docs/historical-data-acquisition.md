# Historical data acquisition

## Purpose

`chronos_databento_historical` acquires the first real market recordings used
by Chronos. It is deliberately narrow: Databento `EQUS.MINI`, MBP-1, raw
equity symbols, and instrument-ID output. The executable supports a free
metadata quote and a guarded download.

The API key is read only from `DATABENTO_API_KEY`. It is never accepted as a
command-line argument, written to a manifest, or printed in an error.

## Build

```sh
cmake -S . -B build-databento \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCHRONOS_ENABLE_DATABENTO_ADAPTER=ON
cmake --build build-databento --parallel
```

To acquire several recordings for an evaluation, use the request-plan
workflow in [real-market-validation.md](real-market-validation.md): it binds
an exact plan, a human authorization with per-recording caps, and a durable
attempt ledger around this tool, and never retries a download that may have
been charged. The commands below are its single-recording building blocks.

## Quote before downloading

Use a completed market interval expressed explicitly in UTC. Databento
applies `--start` (inclusive) and `--end` (exclusive) to each MBP-1 record's
`ts_recv`, not its `ts_event`. This example is
the first ten minutes of the regular US session for AAPL and SPY on September
25, 2026:

```sh
export DATABENTO_API_KEY='replace-with-your-key'

./build-databento/chronos_databento_historical quote \
  --start 2026-09-25T13:30:00Z \
  --end 2026-09-25T13:40:00Z \
  --symbol AAPL \
  --symbol SPY
```

The JSON response contains the estimated provider record count, uncompressed
billable bytes, and cost in US dollars. Databento says metadata calls are free.
An estimate that is not a finite, nonnegative number is refused by the
acquisition code for any provider, before anything is downloaded.
Its cost estimate can overstate requests whose durations are not multiples of
ten minutes, while actual billing is based on bytes delivered. The Chronos
cost limit is therefore a preflight guard against an unexpectedly large
request, not a replacement for provider account limits.

## Download with an explicit cost limit

Raw market data is licensed and ignored by Git. Create its local destination,
then repeat the exact query with a maximum acceptable quoted cost:

```sh
mkdir -p data/raw/databento/equs-mini

./build-databento/chronos_databento_historical download \
  --start 2026-09-25T13:30:00Z \
  --end 2026-09-25T13:40:00Z \
  --symbol AAPL \
  --symbol SPY \
  --output data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst \
  --stream-id databento-equs-mini-mbp1-20260925-1330-1340 \
  --max-cost-usd 0.25
```

`--max-cost-usd` is read as a binary double. The request-plan workflow passes
the largest double that is at most the authorized whole cents, so the limit
the tool enforces never exceeds what a person approved.

The download command:

1. validates the request and refuses existing recording or manifest paths;
2. obtains a fresh metadata quote;
3. stops before the billable request if the quote exceeds the explicit limit;
4. downloads to a unique temporary file in the destination directory;
5. publishes the recording without overwriting another file;
6. streams every record through the Chronos normalizer and validator;
7. hashes the unchanged source and writes a deterministic manifest; and
8. marks both the raw recording and manifest read-only.

If downloading or validation fails, Chronos removes the temporary or newly
published recording. A successful output is never silently replaced, so a
repeated request cannot accidentally incur a duplicate download or destroy
the recording that produced an earlier result.

## Replay the local recording

Replay is offline and does not require an API key or make a Databento request:

```sh
./build-databento/chronos_databento_replay \
  --input data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst \
  --stream-id databento-equs-mini-mbp1-20260925-1330-1340
```

The JSON output includes the independently recomputed recording manifest,
event totals, first and last receive timestamps, final ingest sequence, and
the final trade and top-of-book state for each instrument. Nanosecond
timestamps and fixed-point prices and quantities are emitted as decimal
strings so JSON consumers do not lose integer precision.

## Repository boundary

Never commit API keys, raw DBN files, normalized licensed events, or generated
trading reports. The repository may contain acquisition instructions, code,
synthetic test recordings created at test time, and reviewed provenance
metadata that is legal to redistribute. Check the applicable provider and
exchange terms before sharing any real-data artifact.

## References

- [Databento historical C++ API](https://databento.com/docs/api-reference-historical/timeseries/timeseries-get-range-to-file?historical=cpp&live=cpp)
- [Databento DBN format](https://databento.com/docs/standards-and-conventions/databento-binary-encoding)
- [Databento pricing](https://databento.com/pricing)
