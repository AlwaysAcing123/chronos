-- Durable good-til-cancelled working orders.
--
-- The order journal (paper_order_records) stays the immutable record of
-- each submission: its intent, the observation it used, and how the
-- submission ended. A good-til-cancelled order keeps changing after that,
-- so every executed order now also has:
--
--   paper_orders  the order's current state: status, filled and remaining
--                 quantities, its one fee, and what it reserves. Updated in
--                 place; the journal row it references never changes.
--   paper_fills   every booked fill, append-only. An order has any number
--                 of fills, numbered from 0; only the first pays the fee.
--
-- and paper_events, still append-only, carries each event's market time and
-- the fill it books. Reservations are generated from each order's own
-- columns, so they cannot disagree with it; deferred constraint triggers
-- check at commit that they are covered by cash and shares and that every
-- order's fills add up to its filled quantity and fee.
--
-- Existing data is copied exactly. Every executed order before this
-- migration is immediate-or-cancel: its paper_orders row repeats its
-- journaled outcome, and its one fill keeps its ID and gets fill record ID
-- equal to its journal record ID, so history cursors issued before the
-- migration still page correctly. A journal record ID stays the order's
-- durable priority. Nothing is reinterpreted: statuses, cancellation
-- reasons, and fill IDs keep their spelling.
--
-- The file owns its transaction and is idempotent: once version 4 is
-- recorded it does nothing. It validates the version 3 data before changing
-- anything and raises an exception, which rolls the whole transaction back,
-- if any of it could not be copied without loss. A new volume applies it
-- after 001 to 003; an existing volume needs it applied once by hand (see
-- docs/postgresql-persistence.md). ALTER TABLE takes ACCESS EXCLUSIVE locks,
-- so stop the paper-trading server first.
BEGIN;

DO $migration$
DECLARE
    unmigratable bigint;
BEGIN
    -- An applied migration is never removed, so a positive check needs no
    -- lock.
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 4) THEN
        RETURN;
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM chronos_schema_migrations WHERE version = 3
    ) THEN
        RAISE EXCEPTION 'migration 004 requires migration 003'
            USING ERRCODE = 'object_not_in_prerequisite_state';
    END IF;

    -- Serialize with writers, readers, and a concurrent run of this file.
    -- Every one of them reads or locks paper_accounts before the other
    -- tables, so it is locked first and they wait for it holding nothing
    -- this migration needs. The other two follow in the order migration 001
    -- takes its index locks, paper_events and then paper_order_records, so
    -- a concurrent rerun of 001 cannot complete a lock cycle with this file.
    LOCK TABLE paper_accounts, paper_events, paper_order_records
        IN ACCESS EXCLUSIVE MODE;
    LOCK TABLE chronos_schema_migrations IN SHARE ROW EXCLUSIVE MODE;
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 4) THEN
        RETURN;
    END IF;

    -- Every executed order so far is a declared immediate-or-cancel order.
    SELECT count(*) INTO unmigratable
    FROM paper_order_records
    WHERE processed_by_execution AND NOT coalesce(
        time_in_force = 'ioc' AND
        side IN ('buy', 'sell') AND
        order_type IN ('market', 'limit') AND
        quantity_raw > 0 AND
        (limit_price_raw IS NULL) = (order_type = 'market') AND
        (limit_price_raw IS NULL OR limit_price_raw > 0) AND
        client_order_id <> '' AND
        submitted_at_ns >= 0,
        false
    );
    IF unmigratable > 0 THEN
        RAISE EXCEPTION
            'migration 004: % executed order(s) are not declared '
            'immediate-or-cancel orders', unmigratable
            USING ERRCODE = 'data_exception';
    END IF;

    -- Each execution has exactly one of the three immediate-or-cancel
    -- shapes, used the order's own observation, and its fill is the order's
    -- first. A receipt carries no rejection; a fault names its code. These
    -- are the rules the journal reader enforces, so a row it would refuse
    -- to read is never copied into the new tables.
    SELECT count(*) INTO unmigratable
    FROM paper_order_records
    WHERE processed_by_execution AND NOT coalesce(
        execution_market_ingest_sequence = market_ingest_sequence AND
        CASE outcome_kind
            WHEN 'receipt' THEN
                outcome_code IS NULL AND outcome_field IS NULL AND
                outcome_message IS NULL
            WHEN 'processing_error' THEN
                outcome_code IS NOT NULL AND outcome_field IS NULL AND
                outcome_message IS NOT NULL
            ELSE false
        END AND
        CASE execution_status
            WHEN 'filled' THEN
                fill_id IS NOT NULL AND cancellation_reason IS NULL AND
                unfilled_quantity_raw = 0 AND
                fill_quantity_raw = quantity_raw
            WHEN 'partially_filled_and_cancelled' THEN
                fill_id IS NOT NULL AND
                cancellation_reason = 'displayed_liquidity_exhausted' AND
                unfilled_quantity_raw > 0 AND
                fill_quantity_raw::numeric + unfilled_quantity_raw =
                    quantity_raw
            WHEN 'cancelled' THEN
                fill_id IS NULL AND cancellation_reason IS NOT NULL AND
                unfilled_quantity_raw = quantity_raw
            ELSE false
        END AND (
            fill_id IS NULL OR (
                fill_side = side AND
                fill_id = client_order_id || ':fill:0' AND
                fill_timestamp_ns >= submitted_at_ns
            )
        ),
        false
    );
    IF unmigratable > 0 THEN
        RAISE EXCEPTION
            'migration 004: % executed order(s) have an inconsistent '
            'outcome, quantity, or fill', unmigratable
            USING ERRCODE = 'data_exception';
    END IF;

    -- A fill_applied event belongs to an order whose fill was booked.
    SELECT count(*) INTO unmigratable
    FROM paper_events AS event
    JOIN paper_order_records AS journal
        ON journal.order_record_id = event.order_record_id
    WHERE event.event_type = 'fill_applied' AND NOT (
        journal.outcome_kind = 'receipt' AND journal.fill_id IS NOT NULL
    );
    IF unmigratable > 0 THEN
        RAISE EXCEPTION
            'migration 004: % fill event(s) have no booked fill',
            unmigratable
            USING ERRCODE = 'data_exception';
    END IF;

    -- A submission's execution may now leave a working remainder.
    ALTER TABLE paper_order_records
        DROP CONSTRAINT paper_order_records_execution_status_check;
    ALTER TABLE paper_order_records
        ADD CONSTRAINT paper_order_records_execution_status_check CHECK (
            execution_status IN (
                'filled',
                'partially_filled_and_cancelled',
                'cancelled',
                'working',
                'partially_filled'
            )
        );

    CREATE TABLE paper_orders (
        order_record_id bigint PRIMARY KEY
            REFERENCES paper_order_records (order_record_id),
        account_id text NOT NULL REFERENCES paper_accounts (account_id),
        client_order_id text NOT NULL CHECK (client_order_id <> ''),
        instrument text NOT NULL,
        side text NOT NULL CHECK (side IN ('buy', 'sell')),
        order_type text NOT NULL CHECK (order_type IN ('market', 'limit')),
        time_in_force text NOT NULL CHECK (time_in_force IN ('ioc', 'gtc')),
        limit_price_raw bigint CHECK (limit_price_raw > 0),
        quantity_raw bigint NOT NULL CHECK (quantity_raw > 0),
        filled_quantity_raw bigint NOT NULL CHECK (filled_quantity_raw >= 0),
        remaining_quantity_raw bigint NOT NULL
            CHECK (remaining_quantity_raw >= 0),
        filled_notional_raw bigint NOT NULL CHECK (filled_notional_raw >= 0),
        fill_count integer NOT NULL CHECK (fill_count >= 0),
        -- The flat fee the order's first fill pays.
        order_fee_raw bigint NOT NULL CHECK (order_fee_raw >= 0),
        status text NOT NULL CHECK (
            status IN (
                'working',
                'partially_filled',
                'filled',
                'partially_filled_and_cancelled',
                'cancelled',
                'faulted'
            )
        ),
        cancellation_reason text CHECK (
            cancellation_reason IN (
                'limit_not_marketable',
                'displayed_liquidity_exhausted',
                'user_requested'
            )
        ),
        cancel_request_id text CHECK (cancel_request_id <> ''),
        -- The stream and observation the order was accepted on. Only later
        -- observations of that stream can fill it.
        market_stream_id text NOT NULL,
        accepted_market_ingest_sequence numeric(20, 0) NOT NULL
            CHECK (accepted_market_ingest_sequence >= 0),
        submitted_at_ns bigint NOT NULL CHECK (submitted_at_ns >= 0),
        -- Market time of the latest change: acceptance, a fill, or the
        -- cancellation.
        updated_at_ns bigint NOT NULL,
        created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
        updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
        fee_charged boolean GENERATED ALWAYS AS (filled_quantity_raw > 0)
            STORED,
        cumulative_fee_raw bigint GENERATED ALWAYS AS (
            CASE WHEN filled_quantity_raw > 0 THEN order_fee_raw ELSE 0 END
        ) STORED,
        -- A working buy reserves its limit price times its remainder,
        -- rounded half away from zero like every notional; a working sell
        -- reserves its remainder of shares. Either reserves its fee until
        -- its first fill. Multiplying by 1e-9 is exact, where dividing
        -- would round the quotient before round() does.
        reserved_cash_raw bigint GENERATED ALWAYS AS ((
            CASE WHEN status IN ('working', 'partially_filled') THEN
                CASE WHEN side = 'buy' THEN
                    round(
                        limit_price_raw::numeric * remaining_quantity_raw *
                        0.000000001
                    )
                ELSE 0 END +
                CASE WHEN filled_quantity_raw = 0 THEN order_fee_raw
                ELSE 0 END
            ELSE 0 END
        )::bigint) STORED,
        reserved_quantity_raw bigint GENERATED ALWAYS AS (
            CASE WHEN status IN ('working', 'partially_filled') AND
                      side = 'sell'
                THEN remaining_quantity_raw ELSE 0 END
        ) STORED,
        CONSTRAINT paper_orders_client_order_key
            UNIQUE (account_id, client_order_id),
        -- A cancel request ID cancels at most one order of the account.
        CONSTRAINT paper_orders_cancel_request_key
            UNIQUE (account_id, cancel_request_id),
        -- Lets each fill reference the exact order it fills.
        CONSTRAINT paper_orders_fill_reference_key UNIQUE (
            order_record_id, account_id, client_order_id, instrument, side
        ),
        CONSTRAINT paper_orders_quantities_add_up CHECK (
            filled_quantity_raw::numeric + remaining_quantity_raw =
            quantity_raw
        ),
        CONSTRAINT paper_orders_limit_matches_type CHECK (
            (limit_price_raw IS NULL) = (order_type = 'market')
        ),
        CONSTRAINT paper_orders_market_orders_are_ioc CHECK (
            order_type = 'limit' OR time_in_force = 'ioc'
        ),
        CONSTRAINT paper_orders_fills_match_quantity CHECK (
            (fill_count = 0) = (filled_quantity_raw = 0) AND
            (fill_count = 0) = (filled_notional_raw = 0)
        ),
        CONSTRAINT paper_orders_updated_after_submission CHECK (
            updated_at_ns >= submitted_at_ns
        ),
        CONSTRAINT paper_orders_status_is_consistent CHECK (
            CASE status
                WHEN 'working' THEN
                    time_in_force = 'gtc' AND filled_quantity_raw = 0 AND
                    remaining_quantity_raw > 0 AND cancellation_reason IS NULL
                WHEN 'partially_filled' THEN
                    time_in_force = 'gtc' AND filled_quantity_raw > 0 AND
                    remaining_quantity_raw > 0 AND cancellation_reason IS NULL
                WHEN 'filled' THEN
                    remaining_quantity_raw = 0 AND cancellation_reason IS NULL
                WHEN 'partially_filled_and_cancelled' THEN
                    filled_quantity_raw > 0 AND remaining_quantity_raw > 0 AND
                    cancellation_reason IS NOT NULL
                WHEN 'cancelled' THEN
                    filled_quantity_raw = 0 AND cancellation_reason IS NOT NULL
                WHEN 'faulted' THEN
                    remaining_quantity_raw > 0 AND cancellation_reason IS NULL
                ELSE false
            END
        ),
        -- Clients cancel only good-til-cancelled orders, and every such
        -- cancellation records the request that made it.
        CONSTRAINT paper_orders_cancellation_matches_time_in_force CHECK (
            cancellation_reason IS NULL OR
            (cancellation_reason = 'user_requested') =
                (time_in_force = 'gtc')
        ),
        CONSTRAINT paper_orders_cancel_request_is_user_cancel CHECK (
            (cancel_request_id IS NOT NULL) =
                (cancellation_reason IS NOT DISTINCT FROM 'user_requested')
        )
    );

    INSERT INTO paper_orders (
        order_record_id,
        account_id,
        client_order_id,
        instrument,
        side,
        order_type,
        time_in_force,
        limit_price_raw,
        quantity_raw,
        filled_quantity_raw,
        remaining_quantity_raw,
        filled_notional_raw,
        fill_count,
        order_fee_raw,
        status,
        cancellation_reason,
        cancel_request_id,
        market_stream_id,
        accepted_market_ingest_sequence,
        submitted_at_ns,
        updated_at_ns,
        created_at,
        updated_at
    )
    SELECT
        journal.order_record_id,
        journal.account_id,
        journal.client_order_id,
        journal.instrument,
        journal.side,
        journal.order_type,
        journal.time_in_force,
        journal.limit_price_raw,
        journal.quantity_raw,
        booked.quantity_raw,
        journal.quantity_raw - booked.quantity_raw,
        booked.notional_raw,
        booked.fill_count,
        -- A booked fill paid the order's fee; an order without one would
        -- have paid the account's.
        coalesce(booked.fee_raw, owner.fee_per_fill_raw),
        CASE WHEN journal.outcome_kind = 'receipt'
            THEN journal.execution_status ELSE 'faulted' END,
        CASE WHEN journal.outcome_kind = 'receipt'
            THEN journal.cancellation_reason END,
        NULL,
        journal.market_stream_id,
        journal.market_ingest_sequence,
        journal.submitted_at_ns,
        coalesce(booked.filled_at_ns, journal.submitted_at_ns),
        journal.created_at,
        journal.created_at
    FROM paper_order_records AS journal
    JOIN paper_accounts AS owner ON owner.account_id = journal.account_id
    -- Only a receipt booked its fill; a fault's simulated fill never was.
    CROSS JOIN LATERAL (
        SELECT
            CASE WHEN journal.outcome_kind = 'receipt' AND
                      journal.fill_id IS NOT NULL
                THEN journal.fill_quantity_raw ELSE 0 END AS quantity_raw,
            CASE WHEN journal.outcome_kind = 'receipt' AND
                      journal.fill_id IS NOT NULL
                THEN round(
                    journal.fill_price_raw::numeric *
                    journal.fill_quantity_raw * 0.000000001
                )::bigint ELSE 0 END AS notional_raw,
            CASE WHEN journal.outcome_kind = 'receipt' AND
                      journal.fill_id IS NOT NULL
                THEN 1 ELSE 0 END AS fill_count,
            CASE WHEN journal.outcome_kind = 'receipt'
                THEN journal.fill_fee_raw END AS fee_raw,
            CASE WHEN journal.outcome_kind = 'receipt'
                THEN journal.fill_timestamp_ns END AS filled_at_ns
    ) AS booked
    WHERE journal.processed_by_execution;

    CREATE TABLE paper_fills (
        fill_record_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
        account_id text NOT NULL,
        order_record_id bigint NOT NULL,
        client_order_id text NOT NULL,
        fill_index integer NOT NULL CHECK (fill_index >= 0),
        fill_id text NOT NULL,
        instrument text NOT NULL,
        side text NOT NULL CHECK (side IN ('buy', 'sell')),
        price_raw bigint NOT NULL CHECK (price_raw > 0),
        quantity_raw bigint NOT NULL CHECK (quantity_raw > 0),
        fee_raw bigint NOT NULL CHECK (fee_raw >= 0),
        -- The notional accounting booked for this fill.
        notional_raw bigint NOT NULL CHECK (notional_raw > 0),
        filled_at_ns bigint NOT NULL CHECK (filled_at_ns >= 0),
        -- The observation the fill executed against.
        market_stream_id text NOT NULL,
        market_ingest_sequence numeric(20, 0) NOT NULL
            CHECK (market_ingest_sequence >= 0),
        created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
        CONSTRAINT paper_fills_fill_id_key UNIQUE (account_id, fill_id),
        CONSTRAINT paper_fills_fill_index_key
            UNIQUE (order_record_id, fill_index),
        -- A fill has its order's account, client order ID, instrument, and
        -- side.
        CONSTRAINT paper_fills_match_their_order FOREIGN KEY (
            order_record_id,
            account_id,
            client_order_id,
            instrument,
            side
        ) REFERENCES paper_orders (
            order_record_id,
            account_id,
            client_order_id,
            instrument,
            side
        ),
        CONSTRAINT paper_fills_id_names_order_and_index CHECK (
            fill_id = client_order_id || ':fill:' || fill_index
        ),
        -- One fee per order, paid by its first fill.
        CONSTRAINT paper_fills_only_first_fill_pays_fee CHECK (
            fill_index = 0 OR fee_raw = 0
        ),
        CONSTRAINT paper_fills_notional_is_exact CHECK (
            notional_raw::numeric =
                round(price_raw::numeric * quantity_raw * 0.000000001)
        )
    );

    INSERT INTO paper_fills (
        fill_record_id,
        account_id,
        order_record_id,
        client_order_id,
        fill_index,
        fill_id,
        instrument,
        side,
        price_raw,
        quantity_raw,
        fee_raw,
        notional_raw,
        filled_at_ns,
        market_stream_id,
        market_ingest_sequence,
        created_at
    )
    OVERRIDING SYSTEM VALUE
    SELECT
        order_record_id,
        account_id,
        order_record_id,
        client_order_id,
        0,
        fill_id,
        instrument,
        fill_side,
        fill_price_raw,
        fill_quantity_raw,
        fill_fee_raw,
        round(fill_price_raw::numeric * fill_quantity_raw * 0.000000001)::bigint,
        fill_timestamp_ns,
        market_stream_id,
        execution_market_ingest_sequence,
        created_at
    FROM paper_order_records
    WHERE outcome_kind = 'receipt' AND fill_id IS NOT NULL;
    -- New fills are numbered after every copied one.
    PERFORM setval(
        pg_get_serial_sequence('paper_fills', 'fill_record_id'),
        coalesce((SELECT max(fill_record_id) FROM paper_fills), 0) + 1,
        false
    );

    -- A long-lived order can have more events than a smallint counts.
    ALTER TABLE paper_events ALTER COLUMN event_order TYPE integer;
    ALTER TABLE paper_events
        ADD COLUMN occurred_at_ns bigint,
        ADD COLUMN fill_id text;
    -- Every earlier event was written while its submission was processed.
    UPDATE paper_events AS event
    SET occurred_at_ns = journal.submitted_at_ns,
        fill_id = CASE WHEN event.event_type = 'fill_applied'
            THEN journal.fill_id END
    FROM paper_order_records AS journal
    WHERE journal.order_record_id = event.order_record_id;
    ALTER TABLE paper_events ALTER COLUMN occurred_at_ns SET NOT NULL;
    ALTER TABLE paper_events DROP CONSTRAINT paper_events_event_type_check;
    ALTER TABLE paper_events
        ADD CONSTRAINT paper_events_event_type_check CHECK (
            event_type IN (
                'order_submitted',
                'risk_rejected',
                'execution_rejected',
                'order_completed',
                'fill_applied',
                'account_state_updated',
                'processor_faulted',
                'order_accepted',
                'order_filled',
                'order_cancelled'
            )
        ),
        ADD CONSTRAINT paper_events_fill_matches_type CHECK (
            (fill_id IS NOT NULL) = (event_type = 'fill_applied')
        ),
        ADD CONSTRAINT paper_events_fill_fkey
            FOREIGN KEY (account_id, fill_id)
            REFERENCES paper_fills (account_id, fill_id);

    -- The matcher finds an observation's marketable working orders through
    -- the first index; recovery and the working-orders page read an
    -- account's through the second, in priority order.
    CREATE INDEX paper_orders_working_match_idx
        ON paper_orders (instrument, side, limit_price_raw)
        WHERE status IN ('working', 'partially_filled');
    CREATE INDEX paper_orders_account_working_idx
        ON paper_orders (account_id, order_record_id)
        WHERE status IN ('working', 'partially_filled');
    -- Newest-first fill history, like the journal's pages.
    CREATE INDEX paper_fills_account_fill_idx
        ON paper_fills (account_id, fill_record_id);
    CREATE INDEX paper_fills_account_instrument_fill_idx
        ON paper_fills (account_id, instrument, fill_record_id);

    -- The account identity of migration 003, plus: every working order's
    -- reservation is covered, cash by the account's cash and shares by the
    -- instrument's position.
    CREATE OR REPLACE FUNCTION paper_account_reconciles() RETURNS trigger
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
        IF (
            SELECT coalesce(sum(reserved_cash_raw::numeric), 0)
            FROM paper_orders
            WHERE account_id = checked_account_id AND
                  status IN ('working', 'partially_filled')
        ) > account_row.cash_raw THEN
            RAISE EXCEPTION
                'paper account % reserves more cash than it holds',
                checked_account_id
                USING ERRCODE = 'check_violation',
                      CONSTRAINT = 'paper_account_reconciles';
        END IF;
        IF EXISTS (
            SELECT 1
            FROM (
                SELECT instrument, sum(reserved_quantity_raw::numeric) AS held
                FROM paper_orders
                WHERE account_id = checked_account_id AND
                      status IN ('working', 'partially_filled') AND
                      side = 'sell'
                GROUP BY instrument
            ) AS reserved
            LEFT JOIN paper_positions AS position
                ON position.account_id = checked_account_id AND
                   position.instrument = reserved.instrument
            WHERE reserved.held > coalesce(position.quantity_raw, 0)
        ) THEN
            RAISE EXCEPTION
                'paper account % reserves more shares than it holds',
                checked_account_id
                USING ERRCODE = 'check_violation',
                      CONSTRAINT = 'paper_account_reconciles';
        END IF;
    END LOOP;
    RETURN NULL;
END;
    $function$;

    CREATE CONSTRAINT TRIGGER paper_order_reconciles
        AFTER INSERT OR UPDATE OR DELETE ON paper_orders
        DEFERRABLE INITIALLY DEFERRED
        FOR EACH ROW EXECUTE FUNCTION paper_account_reconciles();

    -- An order's fills add up to its filled quantity, notional, and fee,
    -- and are numbered without gaps.
    CREATE FUNCTION paper_order_fills_reconcile() RETURNS trigger
    LANGUAGE plpgsql AS $function$
DECLARE
    checked_order_id bigint;
    checked_order_ids bigint[];
    order_row record;
    fill_totals record;
BEGIN
    IF TG_OP = 'DELETE' THEN
        checked_order_ids := ARRAY[OLD.order_record_id];
    ELSIF TG_OP = 'UPDATE' AND
          OLD.order_record_id IS DISTINCT FROM NEW.order_record_id THEN
        checked_order_ids := ARRAY[OLD.order_record_id, NEW.order_record_id];
    ELSE
        checked_order_ids := ARRAY[NEW.order_record_id];
    END IF;

    FOREACH checked_order_id IN ARRAY checked_order_ids LOOP
        SELECT filled_quantity_raw, filled_notional_raw, fill_count,
               cumulative_fee_raw
        INTO order_row
        FROM paper_orders
        WHERE order_record_id = checked_order_id;
        IF NOT FOUND THEN
            CONTINUE;
        END IF;
        SELECT
            count(*) AS fills,
            coalesce(sum(quantity_raw::numeric), 0) AS quantity,
            coalesce(sum(notional_raw::numeric), 0) AS notional,
            coalesce(sum(fee_raw::numeric), 0) AS fee,
            coalesce(max(fill_index), -1) AS last_index
        INTO fill_totals
        FROM paper_fills
        WHERE order_record_id = checked_order_id;
        IF fill_totals.fills <> order_row.fill_count OR
           fill_totals.last_index <> order_row.fill_count - 1 OR
           fill_totals.quantity <> order_row.filled_quantity_raw OR
           fill_totals.notional <> order_row.filled_notional_raw OR
           fill_totals.fee <> order_row.cumulative_fee_raw THEN
            RAISE EXCEPTION
                'paper order % disagrees with its fills', checked_order_id
                USING ERRCODE = 'check_violation',
                      CONSTRAINT = 'paper_order_fills_reconcile';
        END IF;
    END LOOP;
    RETURN NULL;
END;
    $function$;

    CREATE CONSTRAINT TRIGGER paper_order_fills_reconcile
        AFTER INSERT OR UPDATE ON paper_orders
        DEFERRABLE INITIALLY DEFERRED
        FOR EACH ROW EXECUTE FUNCTION paper_order_fills_reconcile();
    CREATE CONSTRAINT TRIGGER paper_fill_reconciles
        AFTER INSERT OR UPDATE OR DELETE ON paper_fills
        DEFERRABLE INITIALLY DEFERRED
        FOR EACH ROW EXECUTE FUNCTION paper_order_fills_reconcile();

    -- The copy is exact: every executed order has its row, every booked
    -- fill its row, and every order agrees with its fills. The triggers
    -- above only check rows written after they exist, so check the copy
    -- here.
    SELECT
        (SELECT count(*) FROM paper_order_records
         WHERE processed_by_execution) -
        (SELECT count(*) FROM paper_orders)
    INTO unmigratable;
    IF unmigratable <> 0 THEN
        RAISE EXCEPTION 'migration 004: executed orders were not all copied'
            USING ERRCODE = 'data_exception';
    END IF;
    SELECT
        (SELECT count(*) FROM paper_order_records
         WHERE outcome_kind = 'receipt' AND fill_id IS NOT NULL) -
        (SELECT count(*) FROM paper_fills)
    INTO unmigratable;
    IF unmigratable <> 0 THEN
        RAISE EXCEPTION 'migration 004: booked fills were not all copied'
            USING ERRCODE = 'data_exception';
    END IF;
    SELECT count(*) INTO unmigratable
    FROM paper_orders AS copied
    LEFT JOIN (
        SELECT
            order_record_id,
            count(*) AS fills,
            sum(quantity_raw::numeric) AS quantity,
            sum(notional_raw::numeric) AS notional,
            sum(fee_raw::numeric) AS fee
        FROM paper_fills
        GROUP BY order_record_id
    ) AS totals ON totals.order_record_id = copied.order_record_id
    WHERE coalesce(totals.fills, 0) <> copied.fill_count OR
          coalesce(totals.quantity, 0) <> copied.filled_quantity_raw OR
          coalesce(totals.notional, 0) <> copied.filled_notional_raw OR
          coalesce(totals.fee, 0) <> copied.cumulative_fee_raw;
    IF unmigratable > 0 THEN
        RAISE EXCEPTION
            'migration 004: % copied order(s) disagree with their fills',
            unmigratable
            USING ERRCODE = 'data_exception';
    END IF;
    SELECT count(*) INTO unmigratable
    FROM paper_events
    WHERE occurred_at_ns IS NULL OR
          (event_type = 'fill_applied') <> (fill_id IS NOT NULL);
    IF unmigratable > 0 THEN
        RAISE EXCEPTION
            'migration 004: % event(s) could not be dated or linked',
            unmigratable
            USING ERRCODE = 'data_exception';
    END IF;

    INSERT INTO chronos_schema_migrations (version, description)
    VALUES (
        4,
        'durable good-til-cancelled working orders: order state, fills, '
        'and reservations'
    );
END
$migration$;

COMMIT;
