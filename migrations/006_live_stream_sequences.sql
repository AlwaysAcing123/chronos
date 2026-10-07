-- Restart-safe live-stream identity (ADR 0020).
--
--   live_stream_sequences  one row per live market-data stream: the first
--                          ingest sequence never reserved, and the one server
--                          process that may publish on the stream
--
-- A live source numbers its events from blocks of sequences it reserves here
-- while it holds the stream's writer lease, so no sequence is issued twice or
-- moves backward across restarts, and two processes never publish on one
-- stream at once. next_sequence is the exclusive end of everything ever
-- reserved: every sequence any process published is below it. 2^64 means
-- the stream is exhausted. A lease expires at lease_expires_at, in this
-- database's clock.
--
-- Replays never use this table: their sequences come from their recordings,
-- and they stay deterministic.
--
-- No row is created here. A stream without one starts at 0 when no durable
-- table references it. A stream whose accounts, orders, fills, working
-- orders, or deployments already reference it (written before this
-- migration) is continued only after an explicit adoption, which records
-- 'adopted' and the largest sequence that history references.
--
-- The file owns its transaction and is idempotent: once version 6 is
-- recorded it does nothing. It adds one table and rewrites no data.
BEGIN;

DO $migration$
BEGIN
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 6) THEN
        RETURN;
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM chronos_schema_migrations WHERE version = 5
    ) THEN
        RAISE EXCEPTION 'migration 006 requires migration 005'
            USING ERRCODE = 'object_not_in_prerequisite_state';
    END IF;
    -- Serializes a concurrent rerun; no other table is touched.
    LOCK TABLE chronos_schema_migrations IN SHARE ROW EXCLUSIVE MODE;
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 6) THEN
        RETURN;
    END IF;

    CREATE TABLE live_stream_sequences (
        stream_id text PRIMARY KEY CHECK (stream_id <> ''),
        next_sequence numeric(20, 0) NOT NULL
            CHECK (next_sequence >= 0 AND next_sequence <= 18446744073709551616),
        -- The writer: a random ID per server process, never reused, and
        -- where it runs, for operators. All three are null once released.
        writer_id text CHECK (writer_id <> ''),
        writer_label text,
        lease_expires_at timestamptz,
        origin text NOT NULL CHECK (origin IN ('new', 'adopted')),
        adopted_maximum numeric(20, 0)
            CHECK (adopted_maximum >= 0 AND adopted_maximum <= 18446744073709551615),
        acquisitions bigint NOT NULL DEFAULT 0 CHECK (acquisitions >= 0),
        created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
        updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
        CONSTRAINT live_stream_sequences_lease_complete CHECK (
            (writer_id IS NULL) = (lease_expires_at IS NULL) AND
            (writer_id IS NULL) = (writer_label IS NULL)
        ),
        CONSTRAINT live_stream_sequences_adoption_recorded CHECK (
            (origin = 'adopted') = (adopted_maximum IS NOT NULL) AND
            (origin = 'new' OR next_sequence > adopted_maximum)
        )
    );

    INSERT INTO chronos_schema_migrations (version, description)
    VALUES (
        6,
        'live stream sequences: durable reservations and one writer lease '
        'per live market-data stream'
    );
END
$migration$;

COMMIT;
