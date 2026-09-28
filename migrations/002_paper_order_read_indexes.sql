-- Indexes for the paper-trading read API: newest-first keyset pages of one
-- account's order journal and of its booked fills. Both statements are
-- idempotent, so the file can be applied again to an existing database.
--
-- A plain CREATE INDEX blocks writes to paper_order_records while it builds,
-- which is acceptable for local development and CI. A large production table
-- would use CREATE INDEX CONCURRENTLY, which cannot run inside a transaction.
BEGIN;

CREATE INDEX IF NOT EXISTS paper_order_account_record_idx
    ON paper_order_records (account_id, order_record_id);

-- Only receipts book fills. A processing fault can journal the simulated fill
-- that accounting rejected, so fill_id alone does not identify a booked fill.
CREATE INDEX IF NOT EXISTS paper_order_account_booked_fill_idx
    ON paper_order_records (account_id, order_record_id)
    WHERE outcome_kind = 'receipt' AND fill_id IS NOT NULL;

INSERT INTO chronos_schema_migrations (version, description)
VALUES (2, 'paper order journal read indexes')
ON CONFLICT (version) DO NOTHING;

COMMIT;
