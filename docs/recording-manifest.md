# Recording manifest contract

## Purpose

A Chronos recording manifest binds a completed normalization run to the exact raw DBN file that produced it. It is evidence about provenance and reproducibility, not a claim that the underlying market data may be redistributed.

The manifest becomes available only after the reader has:

1. decoded the complete recording without exceptions or provider warnings;
2. resolved and normalized every record;
3. validated every emitted event and its ingest sequence;
4. confirmed that the source size and modification time did not change; and
5. calculated SHA-256 over the complete source file and rechecked the source snapshot.

An incomplete or rejected read has no manifest.

## Version 1 contents

The deterministic JSON document records:

- manifest and normalization-contract versions;
- raw source file name, byte size, and SHA-256;
- provider, dataset, schema, DBN version, pinned SDK version, and SDK source commit;
- query start and end timestamps, input/output symbology, requested symbols, partial resolutions, unresolved symbols, and send-timestamp presence;
- Chronos stream identity, fixed-point scales, first and last ingest sequences; and
- decoded provider-record and emitted normalized-event counts.

It deliberately stores only the source file name rather than a machine-specific absolute path. The checksum is the stable identity. It also omits a generation timestamp so repeated processing of the same source and configuration produces identical JSON.

## Write-once publication

`write_recording_manifest` serializes the document to a read-only temporary file in the destination directory, synchronizes its contents, and publishes it with an atomic hard link. It never overwrites an existing destination. Concurrent publishers therefore have a single winner, and a crash before publication cannot expose a partially written destination.

Publication failures are structured. A caller must not treat a pre-existing manifest as equivalent without separately reading and comparing it; silent replacement would destroy audit history.

The operating-system owner can still deliberately change permissions or delete the file. "Write once" describes the safe Chronos API and publication operation, not an append-only storage service. Stronger retention controls belong in the later persistence and deployment layers.

## Licensing boundary

The manifest can be committed because it contains provenance and aggregate counts rather than licensed market records. Raw Databento recordings, normalized datasets and feature artifacts derived from them, and screenshots or fixtures derived from licensed data require a separate redistribution review before publication.

A normalized dataset records this manifest's `source.sha256` as its `recording_sha256` and never replaces the recording ([normalized-market-datasets.md](normalized-market-datasets.md)).
