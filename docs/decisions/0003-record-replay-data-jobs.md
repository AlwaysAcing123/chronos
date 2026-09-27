# ADR 0003: Data-job record and replay

- Status: Accepted
- Date: 2026-09-27

## Context

Chronos will eventually fetch more than market ticks: instrument reference data, calendars, corporate actions, model artifacts, and experiment inputs. Direct service calls scattered through business logic would make those computations difficult to test, reproduce, batch, cache, or explain.

Jane Street has publicly described an internal OCaml library named Datafetcher. It represents fetch-and-transform jobs declaratively, batches and caches external requests, and can record production responses for later test replay. A later integration with Incremental allowed jobs to update efficiently when live inputs changed.

Datafetcher is not a public market-data product or an available dependency. It would not replace Databento, decode DBN, or supply Chronos with exchange data.

## Decision

Chronos will keep Databento as its initial market-data provider and treat Datafetcher as an architectural reference only.

The project will adopt the useful semantics incrementally when a concrete second external-data workflow requires them:

- explicit live, record, and replay modes;
- typed request and response boundaries;
- stable request identities and content checksums;
- deterministic replay without network access;
- request deduplication, caching, and provider-supported batching; and
- inspectable dependency lineage from an output back to recorded inputs.

The current DBN reader and recording manifest are the first narrow instance of this approach. They preserve the provider-native recording, normalize it through one boundary, and bind the result to its exact source. They do not introduce a generic free-monad framework, dependency graph, or cache before the project has another real use case.

## Consequences

- Chronos gains the testing and reproducibility benefits of record/replay without depending on unavailable internal software.
- The hot market-data path remains a purpose-built streaming adapter rather than a generic batch-fetch abstraction.
- Later reference-data and research jobs can share consistent record/replay semantics.
- The eventual OCaml implementation may use a more native declarative or free-monad representation while preserving the same externally observable recordings and manifests.
- A generic data-job layer is deliberately deferred until duplication in real integrations justifies it.

## References

- [Jane Street: Memoization in Datafetcher](https://blog.janestreet.com/what-the-interns-have-wrought-2021/)
- [Jane Street: Marrying Datafetcher and Incremental](https://blog.janestreet.com/what-the-interns-have-wrought-2022/)
- [MIT talk description: Datafetcher](https://www.eecs.mit.edu/eecs-events/datafetcher/)
