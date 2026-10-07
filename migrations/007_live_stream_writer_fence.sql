-- Writer generations of live streams: the database fence (ADR 0020, as
-- revised 2026-10-06).
--
--   live_stream_sequences.writer_first_sequence
--       the first sequence of the current writer's generation, set when a
--       writer acquires the stream and cleared when it releases it. The
--       generation owns every sequence from here up to next_sequence; every
--       earlier sequence belongs to an earlier generation.
--
-- An authoritative change that records a live ingest sequence (an order,
-- fill, execution state, model decision, or watermark) checks in its own
-- transaction, under a share lock on the stream's row, that a writer holds
-- the stream and that the sequence belongs to its generation. A takeover, a
-- release, and a reservation update the row, so they wait for every such
-- change in flight: a deposed writer's change either commits before the next
-- generation begins or sees it and is refused. A local lease deadline is
-- not what makes this hold.
--
-- A row that has a writer when this runs gets next_sequence: that writer
-- predates the fence, so nothing it numbered passes until it acquires the
-- stream again. Apply this with every live server on the database stopped.
-- After it, a server from before it cannot acquire or release a stream,
-- because it does not maintain the column; it fails closed. A trigger
-- enforces that a new writer begins its generation exactly where every
-- earlier reservation ended, so no writer can take over a stream and leave
-- the deposed generation's sequences current.
--
-- The file owns its transaction and is idempotent: once version 7 is
-- recorded it does nothing. It adds one column, one constraint, and one
-- trigger, and changes no row that has no writer.
BEGIN;

DO $migration$
BEGIN
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 7) THEN
        RETURN;
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM chronos_schema_migrations WHERE version = 6
    ) THEN
        RAISE EXCEPTION 'migration 007 requires migration 006'
            USING ERRCODE = 'object_not_in_prerequisite_state';
    END IF;
    -- Serializes a concurrent rerun; no other table is touched.
    LOCK TABLE chronos_schema_migrations IN SHARE ROW EXCLUSIVE MODE;
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 7) THEN
        RETURN;
    END IF;

    ALTER TABLE live_stream_sequences
        ADD COLUMN writer_first_sequence numeric(20, 0)
            CHECK (
                writer_first_sequence >= 0 AND
                writer_first_sequence <= 18446744073709551616
            );
    UPDATE live_stream_sequences
    SET writer_first_sequence = next_sequence
    WHERE writer_id IS NOT NULL;
    ALTER TABLE live_stream_sequences
        ADD CONSTRAINT live_stream_sequences_generation_complete CHECK (
            (writer_id IS NULL) = (writer_first_sequence IS NULL) AND
            (writer_first_sequence IS NULL OR writer_first_sequence <= next_sequence)
        );

    CREATE FUNCTION live_stream_generation_begins_after_reservations()
    RETURNS trigger
    LANGUAGE plpgsql AS $function$
BEGIN
    IF NEW.writer_id IS NOT NULL AND
       NEW.writer_id IS DISTINCT FROM OLD.writer_id AND
       NEW.writer_first_sequence IS DISTINCT FROM OLD.next_sequence THEN
        RAISE EXCEPTION 'a new writer of live stream % must begin its generation at %, where every earlier reservation ended',
            OLD.stream_id, OLD.next_sequence
            USING ERRCODE = 'check_violation';
    END IF;
    RETURN NEW;
END
$function$;
    CREATE TRIGGER live_stream_generation_begins_after_reservations
        BEFORE UPDATE ON live_stream_sequences
        FOR EACH ROW EXECUTE FUNCTION live_stream_generation_begins_after_reservations();

    INSERT INTO chronos_schema_migrations (version, description)
    VALUES (
        7,
        'live stream writer generations: the fence that refuses a deposed '
        'writer''s sequences at every authoritative commit'
    );
END
$migration$;

COMMIT;
