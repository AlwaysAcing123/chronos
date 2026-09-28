BEGIN;

CREATE TABLE IF NOT EXISTS chronos_schema_migrations (
    version bigint PRIMARY KEY,
    description text NOT NULL,
    applied_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

CREATE TABLE IF NOT EXISTS paper_accounts (
    account_id text PRIMARY KEY,
    instrument text NOT NULL,
    starting_cash_raw bigint NOT NULL CHECK (starting_cash_raw >= 0),
    cash_raw bigint NOT NULL CHECK (cash_raw >= 0),
    position_raw bigint NOT NULL CHECK (position_raw >= 0),
    open_cost_basis_raw bigint NOT NULL CHECK (open_cost_basis_raw >= 0),
    gross_realized_pnl_raw bigint NOT NULL,
    fees_paid_raw bigint NOT NULL CHECK (fees_paid_raw >= 0),
    last_fill_timestamp_ns bigint CHECK (last_fill_timestamp_ns >= 0),
    maximum_order_quantity_raw bigint NOT NULL
        CHECK (maximum_order_quantity_raw > 0),
    maximum_order_notional_raw bigint NOT NULL
        CHECK (maximum_order_notional_raw > 0),
    maximum_position_quantity_raw bigint NOT NULL
        CHECK (maximum_position_quantity_raw > 0),
    maximum_quote_age_ns bigint NOT NULL CHECK (maximum_quote_age_ns >= 0),
    fee_per_fill_raw bigint NOT NULL CHECK (fee_per_fill_raw >= 0),
    is_healthy boolean NOT NULL,
    active_stream_id text,
    active_venue text,
    active_event_timestamp_ns bigint,
    active_receive_timestamp_ns bigint,
    active_ingest_sequence numeric(20, 0),
    active_venue_sequence numeric(20, 0),
    active_quality_flags bigint,
    active_bid_price_raw bigint,
    active_bid_quantity_raw bigint,
    active_ask_price_raw bigint,
    active_ask_quantity_raw bigint,
    consumed_bid_quantity_raw bigint NOT NULL DEFAULT 0
        CHECK (consumed_bid_quantity_raw >= 0),
    consumed_ask_quantity_raw bigint NOT NULL DEFAULT 0
        CHECK (consumed_ask_quantity_raw >= 0),
    version bigint NOT NULL DEFAULT 0 CHECK (version >= 0),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CHECK ((position_raw = 0) = (open_cost_basis_raw = 0)),
    CHECK (
        cash_raw::numeric + open_cost_basis_raw::numeric =
        starting_cash_raw::numeric + gross_realized_pnl_raw::numeric -
        fees_paid_raw::numeric
    ),
    CHECK (
        (active_bid_price_raw IS NULL) =
        (active_bid_quantity_raw IS NULL)
    ),
    CHECK (
        (active_ask_price_raw IS NULL) =
        (active_ask_quantity_raw IS NULL)
    ),
    CHECK (
        active_stream_id IS NOT NULL OR (
            active_venue IS NULL AND
            active_event_timestamp_ns IS NULL AND
            active_receive_timestamp_ns IS NULL AND
            active_ingest_sequence IS NULL AND
            active_venue_sequence IS NULL AND
            active_quality_flags IS NULL AND
            active_bid_price_raw IS NULL AND
            active_bid_quantity_raw IS NULL AND
            active_ask_price_raw IS NULL AND
            active_ask_quantity_raw IS NULL AND
            consumed_bid_quantity_raw = 0 AND
            consumed_ask_quantity_raw = 0
        )
    ),
    CHECK (
        active_stream_id IS NULL OR (
            active_event_timestamp_ns >= 0 AND
            active_receive_timestamp_ns >= 0 AND
            active_ingest_sequence >= 0 AND
            active_quality_flags >= 0
        )
    ),
    CHECK (
        active_bid_quantity_raw IS NOT NULL OR
        consumed_bid_quantity_raw = 0
    ),
    CHECK (
        active_ask_quantity_raw IS NOT NULL OR
        consumed_ask_quantity_raw = 0
    ),
    CHECK (
        active_bid_quantity_raw IS NULL OR
        consumed_bid_quantity_raw <= active_bid_quantity_raw
    ),
    CHECK (
        active_ask_quantity_raw IS NULL OR
        consumed_ask_quantity_raw <= active_ask_quantity_raw
    )
);

CREATE TABLE IF NOT EXISTS paper_order_records (
    order_record_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    account_id text NOT NULL REFERENCES paper_accounts(account_id),
    client_order_id text NOT NULL,
    intent_account_id text NOT NULL,
    instrument text NOT NULL,
    source_kind text NOT NULL,
    source_id text NOT NULL,
    side text NOT NULL,
    order_type text NOT NULL,
    quantity_raw bigint NOT NULL,
    limit_price_raw bigint,
    time_in_force text NOT NULL,
    submitted_at_ns bigint NOT NULL,
    market_instrument text NOT NULL,
    market_stream_id text NOT NULL,
    market_venue text,
    market_event_timestamp_ns bigint NOT NULL,
    market_receive_timestamp_ns bigint NOT NULL,
    market_ingest_sequence numeric(20, 0) NOT NULL
        CHECK (market_ingest_sequence >= 0),
    market_venue_sequence numeric(20, 0),
    market_quality_flags bigint NOT NULL CHECK (market_quality_flags >= 0),
    market_bid_price_raw bigint,
    market_bid_quantity_raw bigint,
    market_ask_price_raw bigint,
    market_ask_quantity_raw bigint,
    outcome_kind text NOT NULL CHECK (
        outcome_kind IN (
            'receipt',
            'risk_rejection',
            'execution_error',
            'processing_error'
        )
    ),
    outcome_code text,
    outcome_field text,
    outcome_message text,
    execution_status text CHECK (
        execution_status IN (
            'filled',
            'partially_filled_and_cancelled',
            'cancelled'
        )
    ),
    unfilled_quantity_raw bigint CHECK (unfilled_quantity_raw >= 0),
    cancellation_reason text CHECK (
        cancellation_reason IN (
            'limit_not_marketable',
            'displayed_liquidity_exhausted'
        )
    ),
    execution_market_ingest_sequence numeric(20, 0),
    fill_id text,
    fill_side text CHECK (fill_side IN ('buy', 'sell')),
    fill_price_raw bigint,
    fill_quantity_raw bigint,
    fill_fee_raw bigint,
    fill_timestamp_ns bigint,
    account_cash_after_raw bigint NOT NULL
        CHECK (account_cash_after_raw >= 0),
    account_position_after_raw bigint NOT NULL
        CHECK (account_position_after_raw >= 0),
    account_open_cost_basis_after_raw bigint NOT NULL
        CHECK (account_open_cost_basis_after_raw >= 0),
    account_gross_realized_pnl_after_raw bigint NOT NULL,
    account_fees_paid_after_raw bigint NOT NULL
        CHECK (account_fees_paid_after_raw >= 0),
    account_last_fill_timestamp_after_ns bigint,
    processed_by_execution boolean NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CHECK (
        (market_bid_price_raw IS NULL) =
        (market_bid_quantity_raw IS NULL)
    ),
    CHECK (
        (market_ask_price_raw IS NULL) =
        (market_ask_quantity_raw IS NULL)
    ),
    CHECK (
        (fill_id IS NULL AND fill_side IS NULL AND
         fill_price_raw IS NULL AND fill_quantity_raw IS NULL AND
         fill_fee_raw IS NULL AND fill_timestamp_ns IS NULL) OR
        (fill_id IS NOT NULL AND fill_side IS NOT NULL AND
         fill_price_raw > 0 AND fill_quantity_raw > 0 AND
         fill_fee_raw >= 0 AND fill_timestamp_ns >= 0)
    ),
    CHECK (
        outcome_kind <> 'receipt' OR (
            execution_status IS NOT NULL AND
            unfilled_quantity_raw IS NOT NULL AND
            execution_market_ingest_sequence IS NOT NULL AND
            account_cash_after_raw >= 0 AND
            account_position_after_raw >= 0 AND
            account_open_cost_basis_after_raw >= 0 AND
            account_fees_paid_after_raw >= 0
        )
    ),
    CHECK (
        outcome_kind = 'receipt' OR outcome_code IS NOT NULL
    ),
    CHECK (
        processed_by_execution = (execution_status IS NOT NULL)
    ),
    CHECK (
        fill_id IS NULL OR processed_by_execution
    )
);

CREATE TABLE IF NOT EXISTS paper_events (
    event_sequence bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    account_id text NOT NULL REFERENCES paper_accounts(account_id),
    order_record_id bigint NOT NULL
        REFERENCES paper_order_records(order_record_id),
    client_order_id text NOT NULL,
    event_order smallint NOT NULL CHECK (event_order > 0),
    event_type text NOT NULL CHECK (
        event_type IN (
            'order_submitted',
            'risk_rejected',
            'execution_rejected',
            'order_completed',
            'fill_applied',
            'account_state_updated',
            'processor_faulted'
        )
    ),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (order_record_id, event_order)
);

CREATE INDEX IF NOT EXISTS paper_events_account_sequence_idx
    ON paper_events (account_id, event_sequence);

CREATE UNIQUE INDEX IF NOT EXISTS paper_order_applied_fill_idx
    ON paper_order_records (account_id, fill_id)
    WHERE outcome_kind = 'receipt' AND fill_id IS NOT NULL;

CREATE UNIQUE INDEX IF NOT EXISTS paper_order_client_idempotency_idx
    ON paper_order_records (account_id, client_order_id)
    WHERE client_order_id <> '';

INSERT INTO chronos_schema_migrations (version, description)
VALUES (1, 'paper trading account snapshots and immutable order journal')
ON CONFLICT (version) DO NOTHING;

COMMIT;
