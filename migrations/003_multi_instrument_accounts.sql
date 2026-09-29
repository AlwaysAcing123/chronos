-- Multi-instrument paper accounts.
--
-- An account keeps one cash balance and a ledger per traded instrument. The
-- single-instrument columns of paper_accounts move to two tables keyed by
-- (account_id, instrument):
--
--   paper_execution_states  the latest execution observation and the
--                           displayed liquidity consumed from it
--   paper_positions         the position ledger, created by the first fill
--                           and retained at zero quantity once closed
--
-- Every existing account keeps its ID, cash, version, risk limits, health,
-- journal, and events unchanged. Its one instrument becomes its only ledger
-- and execution state, with the same raw values. The journal's account-level
-- "after" columns become position-level columns, and the instrument they
-- describe is recorded explicitly.
--
-- The file owns its transaction and is idempotent: once version 3 is
-- recorded it does nothing. It validates the old data before changing
-- anything and raises an exception, which rolls the whole transaction back,
-- if an account cannot be migrated without loss. A new volume applies it
-- after 001 and 002; an existing volume needs it applied once by hand (see
-- docs/postgresql-persistence.md). ALTER TABLE takes ACCESS EXCLUSIVE locks,
-- so stop the paper-trading server first.
BEGIN;

DO $migration$
DECLARE
    unmigratable bigint;
BEGIN
    -- An applied migration is never removed, so a positive check needs no
    -- lock.
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 3) THEN
        RETURN;
    END IF;

    -- Serialize with writers and with a concurrent run of this file. A paper
    -- order changes its account before appending its order record and events,
    -- so take table locks in that same order. This avoids a cycle with an old
    -- server that was accidentally left running while the migration starts.
    LOCK TABLE paper_accounts, paper_order_records, paper_events
        IN ACCESS EXCLUSIVE MODE;
    LOCK TABLE chronos_schema_migrations IN SHARE ROW EXCLUSIVE MODE;
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 3) THEN
        RETURN;
    END IF;

    -- A ledger is created by the first fill, so an account without a fill
    -- must have no position state to carry over.
    SELECT count(*) INTO unmigratable
    FROM paper_accounts
    WHERE last_fill_timestamp_ns IS NULL
      AND (position_raw <> 0 OR open_cost_basis_raw <> 0 OR
           gross_realized_pnl_raw <> 0 OR fees_paid_raw <> 0);
    IF unmigratable > 0 THEN
        RAISE EXCEPTION
            'migration 003: % account(s) without a fill carry position state',
            unmigratable
            USING ERRCODE = 'data_exception';
    END IF;

    -- Every fill executed against the account's active market, so a ledger
    -- always has an execution state.
    SELECT count(*) INTO unmigratable
    FROM paper_accounts
    WHERE last_fill_timestamp_ns IS NOT NULL AND active_stream_id IS NULL;
    IF unmigratable > 0 THEN
        RAISE EXCEPTION
            'migration 003: % account(s) with fills have no execution state',
            unmigratable
            USING ERRCODE = 'data_exception';
    END IF;

    -- The stream an account executes on. Every execution state references
    -- it, so an account's instruments cannot use different streams.
    ALTER TABLE paper_accounts ADD COLUMN market_stream_id text;
    UPDATE paper_accounts SET market_stream_id = active_stream_id
    WHERE active_stream_id IS NOT NULL;
    ALTER TABLE paper_accounts
        ADD CONSTRAINT paper_accounts_market_stream_key
        UNIQUE (account_id, market_stream_id);

    CREATE TABLE paper_execution_states (
        account_id text NOT NULL,
        instrument text NOT NULL,
        market_stream_id text NOT NULL,
        market_venue text,
        market_event_timestamp_ns bigint NOT NULL
            CHECK (market_event_timestamp_ns >= 0),
        market_receive_timestamp_ns bigint NOT NULL
            CHECK (market_receive_timestamp_ns >= 0),
        market_ingest_sequence numeric(20, 0) NOT NULL
            CHECK (market_ingest_sequence >= 0),
        market_venue_sequence numeric(20, 0),
        market_quality_flags bigint NOT NULL
            CHECK (market_quality_flags >= 0),
        market_bid_price_raw bigint,
        market_bid_quantity_raw bigint,
        market_ask_price_raw bigint,
        market_ask_quantity_raw bigint,
        consumed_bid_quantity_raw bigint NOT NULL
            CHECK (consumed_bid_quantity_raw >= 0),
        consumed_ask_quantity_raw bigint NOT NULL
            CHECK (consumed_ask_quantity_raw >= 0),
        PRIMARY KEY (account_id, instrument),
        FOREIGN KEY (account_id, market_stream_id)
            REFERENCES paper_accounts (account_id, market_stream_id),
        CHECK (
            (market_bid_price_raw IS NULL) =
            (market_bid_quantity_raw IS NULL)
        ),
        CHECK (
            (market_ask_price_raw IS NULL) =
            (market_ask_quantity_raw IS NULL)
        ),
        CHECK (
            market_bid_quantity_raw IS NOT NULL OR
            consumed_bid_quantity_raw = 0
        ),
        CHECK (
            market_ask_quantity_raw IS NOT NULL OR
            consumed_ask_quantity_raw = 0
        ),
        CHECK (
            market_bid_quantity_raw IS NULL OR
            consumed_bid_quantity_raw <= market_bid_quantity_raw
        ),
        CHECK (
            market_ask_quantity_raw IS NULL OR
            consumed_ask_quantity_raw <= market_ask_quantity_raw
        )
    );

    INSERT INTO paper_execution_states (
        account_id,
        instrument,
        market_stream_id,
        market_venue,
        market_event_timestamp_ns,
        market_receive_timestamp_ns,
        market_ingest_sequence,
        market_venue_sequence,
        market_quality_flags,
        market_bid_price_raw,
        market_bid_quantity_raw,
        market_ask_price_raw,
        market_ask_quantity_raw,
        consumed_bid_quantity_raw,
        consumed_ask_quantity_raw
    )
    SELECT
        account_id,
        instrument,
        active_stream_id,
        active_venue,
        active_event_timestamp_ns,
        active_receive_timestamp_ns,
        active_ingest_sequence,
        active_venue_sequence,
        active_quality_flags,
        active_bid_price_raw,
        active_bid_quantity_raw,
        active_ask_price_raw,
        active_ask_quantity_raw,
        consumed_bid_quantity_raw,
        consumed_ask_quantity_raw
    FROM paper_accounts
    WHERE active_stream_id IS NOT NULL;

    -- A closed position keeps its ledger with zero quantity and zero open
    -- cost basis, so its realized PnL, fees, and fill time remain.
    CREATE TABLE paper_positions (
        account_id text NOT NULL,
        instrument text NOT NULL,
        quantity_raw bigint NOT NULL CHECK (quantity_raw >= 0),
        open_cost_basis_raw bigint NOT NULL
            CHECK (open_cost_basis_raw >= 0),
        gross_realized_pnl_raw bigint NOT NULL,
        fees_paid_raw bigint NOT NULL CHECK (fees_paid_raw >= 0),
        last_fill_timestamp_ns bigint NOT NULL
            CHECK (last_fill_timestamp_ns >= 0),
        PRIMARY KEY (account_id, instrument),
        FOREIGN KEY (account_id, instrument)
            REFERENCES paper_execution_states (account_id, instrument),
        CHECK ((quantity_raw = 0) = (open_cost_basis_raw = 0))
    );

    INSERT INTO paper_positions (
        account_id,
        instrument,
        quantity_raw,
        open_cost_basis_raw,
        gross_realized_pnl_raw,
        fees_paid_raw,
        last_fill_timestamp_ns
    )
    SELECT
        account_id,
        instrument,
        position_raw,
        open_cost_basis_raw,
        gross_realized_pnl_raw,
        fees_paid_raw,
        last_fill_timestamp_ns
    FROM paper_accounts
    WHERE last_fill_timestamp_ns IS NOT NULL;

    -- The journal's snapshot of state after each order describes the
    -- account's cash and the position in one instrument: the account's only
    -- instrument for existing rows, the order's instrument from now on.
    ALTER TABLE paper_order_records ADD COLUMN position_after_instrument text;
    UPDATE paper_order_records AS journal
    SET position_after_instrument = owner.instrument
    FROM paper_accounts AS owner
    WHERE owner.account_id = journal.account_id;
    ALTER TABLE paper_order_records
        ALTER COLUMN position_after_instrument SET NOT NULL;
    ALTER TABLE paper_order_records
        RENAME COLUMN account_position_after_raw
        TO position_quantity_after_raw;
    ALTER TABLE paper_order_records
        RENAME COLUMN account_open_cost_basis_after_raw
        TO position_open_cost_basis_after_raw;
    ALTER TABLE paper_order_records
        RENAME COLUMN account_gross_realized_pnl_after_raw
        TO position_gross_realized_pnl_after_raw;
    ALTER TABLE paper_order_records
        RENAME COLUMN account_fees_paid_after_raw
        TO position_fees_paid_after_raw;
    ALTER TABLE paper_order_records
        RENAME COLUMN account_last_fill_timestamp_after_ns
        TO position_last_fill_timestamp_after_ns;
    -- A renamed column keeps its constraint; the constraint's name follows.
    ALTER TABLE paper_order_records
        RENAME CONSTRAINT paper_order_records_account_position_after_raw_check
        TO paper_order_records_position_quantity_after_raw_check;
    ALTER TABLE paper_order_records
        RENAME CONSTRAINT
            paper_order_records_account_open_cost_basis_after_raw_check
        TO paper_order_records_position_open_cost_basis_after_raw_check;
    ALTER TABLE paper_order_records
        RENAME CONSTRAINT paper_order_records_account_fees_paid_after_raw_check
        TO paper_order_records_position_fees_paid_after_raw_check;

    -- History filtered to one instrument pages newest first through these,
    -- like the unfiltered pages in 002. Without them the filter reads every
    -- journal row of the account: on a 50,000-order account, a 40-row SPY
    -- page read about 1,900 buffers instead of 5 (see
    -- docs/postgresql-persistence.md).
    CREATE INDEX paper_order_account_instrument_record_idx
        ON paper_order_records (account_id, instrument, order_record_id);
    CREATE INDEX paper_order_account_instrument_booked_fill_idx
        ON paper_order_records (account_id, instrument, order_record_id)
        WHERE outcome_kind = 'receipt' AND fill_id IS NOT NULL;

    -- The copied columns now live only in the new tables. Dropping a column
    -- also drops the CHECK constraints that mention it, including the
    -- single-row accounting identity that the trigger below replaces.
    ALTER TABLE paper_accounts
        DROP COLUMN instrument,
        DROP COLUMN position_raw,
        DROP COLUMN open_cost_basis_raw,
        DROP COLUMN gross_realized_pnl_raw,
        DROP COLUMN fees_paid_raw,
        DROP COLUMN last_fill_timestamp_ns,
        DROP COLUMN active_stream_id,
        DROP COLUMN active_venue,
        DROP COLUMN active_event_timestamp_ns,
        DROP COLUMN active_receive_timestamp_ns,
        DROP COLUMN active_ingest_sequence,
        DROP COLUMN active_venue_sequence,
        DROP COLUMN active_quality_flags,
        DROP COLUMN active_bid_price_raw,
        DROP COLUMN active_bid_quantity_raw,
        DROP COLUMN active_ask_price_raw,
        DROP COLUMN active_ask_quantity_raw,
        DROP COLUMN consumed_bid_quantity_raw,
        DROP COLUMN consumed_ask_quantity_raw;

    -- The account identity spans the account row and its ledgers:
    --   cash + sum(open cost basis)
    --     = starting cash + sum(gross realized PnL) - sum(fees paid)
    -- and an account is bound to a stream exactly when it has an execution
    -- state. Deferred constraint triggers check both at commit, after every
    -- row an order changes has been written.
    CREATE FUNCTION paper_account_reconciles() RETURNS trigger
    LANGUAGE plpgsql AS $function$
DECLARE
    checked_account_id text;
    checked_account_ids text[];
    account_row record;
    ledger_totals record;
BEGIN
    IF TG_OP = 'DELETE' THEN
        checked_account_ids := ARRAY[OLD.account_id];
    ELSIF TG_OP = 'UPDATE' AND
          OLD.account_id IS DISTINCT FROM NEW.account_id THEN
        checked_account_ids := ARRAY[OLD.account_id, NEW.account_id];
    ELSE
        checked_account_ids := ARRAY[NEW.account_id];
    END IF;

    FOREACH checked_account_id IN ARRAY checked_account_ids LOOP
        SELECT starting_cash_raw, cash_raw, market_stream_id
        INTO account_row
        FROM paper_accounts
        WHERE account_id = checked_account_id;
        IF NOT FOUND THEN
            CONTINUE;
        END IF;
        SELECT
            coalesce(sum(open_cost_basis_raw::numeric), 0) AS open_cost_basis,
            coalesce(sum(gross_realized_pnl_raw::numeric), 0)
                AS gross_realized_pnl,
            coalesce(sum(fees_paid_raw::numeric), 0) AS fees_paid
        INTO ledger_totals
        FROM paper_positions
        WHERE account_id = checked_account_id;
        IF account_row.cash_raw::numeric + ledger_totals.open_cost_basis <>
            account_row.starting_cash_raw::numeric +
            ledger_totals.gross_realized_pnl - ledger_totals.fees_paid THEN
            RAISE EXCEPTION
                'paper account % violates the accounting identity',
                checked_account_id
                USING ERRCODE = 'check_violation',
                      CONSTRAINT = 'paper_account_reconciles';
        END IF;
        IF (account_row.market_stream_id IS NULL) <> NOT EXISTS (
            SELECT 1 FROM paper_execution_states
            WHERE account_id = checked_account_id
        ) THEN
            RAISE EXCEPTION
                'paper account % stream binding disagrees with its executions',
                checked_account_id
                USING ERRCODE = 'check_violation',
                      CONSTRAINT = 'paper_account_reconciles';
        END IF;
    END LOOP;
    RETURN NULL;
END;
    $function$;

    CREATE CONSTRAINT TRIGGER paper_account_reconciles
        AFTER INSERT OR UPDATE ON paper_accounts
        DEFERRABLE INITIALLY DEFERRED
        FOR EACH ROW EXECUTE FUNCTION paper_account_reconciles();
    CREATE CONSTRAINT TRIGGER paper_position_reconciles
        AFTER INSERT OR UPDATE OR DELETE ON paper_positions
        DEFERRABLE INITIALLY DEFERRED
        FOR EACH ROW EXECUTE FUNCTION paper_account_reconciles();
    CREATE CONSTRAINT TRIGGER paper_execution_state_reconciles
        AFTER INSERT OR UPDATE OR DELETE ON paper_execution_states
        DEFERRABLE INITIALLY DEFERRED
        FOR EACH ROW EXECUTE FUNCTION paper_account_reconciles();

    -- The copy is exact, so every migrated account must still reconcile.
    SELECT count(*) INTO unmigratable
    FROM paper_accounts AS owner
    LEFT JOIN (
        SELECT
            account_id,
            sum(open_cost_basis_raw::numeric) AS open_cost_basis,
            sum(gross_realized_pnl_raw::numeric) AS gross_realized_pnl,
            sum(fees_paid_raw::numeric) AS fees_paid
        FROM paper_positions
        GROUP BY account_id
    ) AS ledger ON ledger.account_id = owner.account_id
    WHERE owner.cash_raw::numeric + coalesce(ledger.open_cost_basis, 0) <>
        owner.starting_cash_raw::numeric +
        coalesce(ledger.gross_realized_pnl, 0) -
        coalesce(ledger.fees_paid, 0);
    IF unmigratable > 0 THEN
        RAISE EXCEPTION
            'migration 003: % migrated account(s) do not reconcile',
            unmigratable
            USING ERRCODE = 'data_exception';
    END IF;

    INSERT INTO chronos_schema_migrations (version, description)
    VALUES (
        3,
        'multi-instrument paper accounts: per-instrument positions and '
        'execution state'
    );
END
$migration$;

COMMIT;
