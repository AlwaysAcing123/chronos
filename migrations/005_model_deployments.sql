-- Controlled model-driven paper deployments (ADR 0018).
--
--   model_deployments           one verified model, instrument, policy, and
--                               configuration, bound for ever to one new,
--                               dedicated paper account it owns
--   model_deployment_decisions  every decision a deployment made, written
--                               before anything acts on it
--
-- Ownership is the deployment row: an account referenced by a deployment
-- belongs to it, and model_deployments.account_id is unique, so an account
-- belongs to at most one deployment, ever. Existing accounts are untouched:
-- none is referenced by a deployment, so all stay manual.
--
-- A trigger refuses, in the database itself, any order journaled into a
-- deployment's account unless it is that deployment's model order for a
-- decision still pending, so even another process that bypasses the
-- server's ownership checks cannot commit without resolving the same
-- frozen decision and execution in its transaction. This guards ordinary
-- writers, not hostile clients with authority to alter database rows.
--
-- A deployment order and its decision's resolution commit in one
-- transaction, so a pending decision never has an order; a partial unique
-- index allows one pending decision per deployment.
--
-- The file owns its transaction and is idempotent: once version 5 is
-- recorded it does nothing. It adds tables and triggers, rewriting no data;
-- the lock it takes on paper_accounts is for the new foreign key.
BEGIN;

DO $migration$
BEGIN
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 5) THEN
        RETURN;
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM chronos_schema_migrations WHERE version = 4
    ) THEN
        RAISE EXCEPTION 'migration 005 requires migration 004'
            USING ERRCODE = 'object_not_in_prerequisite_state';
    END IF;
    -- As migration 004: paper_accounts first, as every writer locks it
    -- first, then the migration table to serialize a concurrent rerun.
    LOCK TABLE paper_accounts IN SHARE ROW EXCLUSIVE MODE;
    LOCK TABLE chronos_schema_migrations IN SHARE ROW EXCLUSIVE MODE;
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 5) THEN
        RETURN;
    END IF;

    CREATE TABLE model_deployments (
        deployment_id text PRIMARY KEY
            CHECK (deployment_id ~ '^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$'),
        -- Newest-first listing and keyset cursors.
        creation_sequence bigint GENERATED ALWAYS AS IDENTITY UNIQUE,
        account_id text NOT NULL UNIQUE REFERENCES paper_accounts (account_id),
        -- The canonical creation request, so a repeated create is recognized.
        request_sha256 text NOT NULL CHECK (request_sha256 ~ '^[0-9a-f]{64}$'),
        experiment_id text NOT NULL CHECK (experiment_id ~ '^[0-9a-f]{64}$'),
        parameters_sha256 text NOT NULL
            CHECK (parameters_sha256 ~ '^[0-9a-f]{64}$'),
        model_name text NOT NULL CHECK (model_name IN ('logistic', 'majority')),
        instrument text NOT NULL CHECK (instrument <> ''),
        model_feature_set text NOT NULL
            CHECK (model_feature_set = 'top_of_book_baseline/1'),
        policy text NOT NULL CHECK (policy = 'long_flat_threshold/1'),
        threshold_ppb bigint NOT NULL
            CHECK (threshold_ppb BETWEEN 0 AND 1000000000),
        -- At most one share in this first version.
        target_quantity_raw bigint NOT NULL
            CHECK (target_quantity_raw > 0 AND target_quantity_raw <= 100000000),
        decision_interval_ns bigint NOT NULL CHECK (decision_interval_ns > 0),
        latency_ns bigint NOT NULL CHECK (latency_ns > 0),
        max_signal_age_ns bigint NOT NULL,
        max_orders_per_run integer NOT NULL CHECK (max_orders_per_run > 0),
        max_consecutive_rejections integer NOT NULL
            CHECK (max_consecutive_rejections > 0),
        -- No decision on an observation received before this.
        training_cutoff_ns bigint NOT NULL CHECK (training_cutoff_ns >= 0),
        -- The only stream the deployment may trade on.
        stream_id text NOT NULL CHECK (stream_id <> ''),
        state text NOT NULL
            CHECK (state IN ('stopped', 'running', 'stopping', 'faulted')),
        state_reason text NOT NULL CHECK (state_reason <> ''),
        state_changed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
        -- Incremented by every start; a decision belongs to one generation.
        run_generation bigint NOT NULL DEFAULT 0 CHECK (run_generation >= 0),
        next_decision_sequence bigint NOT NULL DEFAULT 1
            CHECK (next_decision_sequence > 0),
        -- The latest observation decided on. Never reset, so a replay
        -- restart or reconnect cannot make an old observation current.
        watermark_receive_ns bigint,
        watermark_ingest_sequence numeric(20, 0),
        orders_this_run integer NOT NULL DEFAULT 0 CHECK (orders_this_run >= 0),
        consecutive_rejections integer NOT NULL DEFAULT 0
            CHECK (consecutive_rejections >= 0),
        version bigint NOT NULL DEFAULT 0 CHECK (version >= 0),
        created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
        CONSTRAINT model_deployments_signal_outlives_latency
            CHECK (max_signal_age_ns > latency_ns),
        CONSTRAINT model_deployments_watermark_is_complete
            CHECK ((watermark_receive_ns IS NULL) = (watermark_ingest_sequence IS NULL))
    );

    CREATE TABLE model_deployment_decisions (
        deployment_id text NOT NULL REFERENCES model_deployments (deployment_id),
        decision_sequence bigint NOT NULL CHECK (decision_sequence > 0),
        run_generation bigint NOT NULL CHECK (run_generation > 0),
        -- The exact observation the signal used.
        stream_id text NOT NULL,
        instrument text NOT NULL,
        observation_ingest_sequence numeric(20, 0) NOT NULL
            CHECK (observation_ingest_sequence >= 0),
        observation_event_ns bigint NOT NULL,
        observation_receive_ns bigint NOT NULL CHECK (observation_receive_ns >= 0),
        -- The market clock when the decision was recorded.
        decided_at_market_ns bigint NOT NULL,
        inputs_raw text[] NOT NULL CHECK (cardinality(inputs_raw) = 5),
        predicted_class text NOT NULL
            CHECK (predicted_class IN ('down', 'flat', 'up')),
        probabilities text[] NOT NULL CHECK (cardinality(probabilities) = 3),
        confidence_ppb bigint NOT NULL
            CHECK (confidence_ppb BETWEEN 0 AND 1000000000),
        position_before_raw bigint NOT NULL CHECK (position_before_raw >= 0),
        target_raw bigint CHECK (target_raw >= 0),
        policy_reason text NOT NULL CHECK (policy_reason IN (
            'hold_flat_prediction', 'hold_below_threshold', 'order_pending',
            'already_at_target', 'order_scheduled'
        )),
        -- An order decision's intended order.
        client_order_id text,
        side text CHECK (side IN ('buy', 'sell')),
        quantity_raw bigint CHECK (quantity_raw > 0),
        not_before_receive_ns bigint,
        expires_at_market_ns bigint,
        -- held: no order intended. pending: an order is intended and nothing
        -- is recorded yet. recorded: its order is in the journal.
        -- abandoned: no order was or ever will be recorded for it.
        state text NOT NULL
            CHECK (state IN ('held', 'pending', 'recorded', 'abandoned')),
        -- recorded: the order's status; abandoned: why.
        resolution text,
        execution_ingest_sequence numeric(20, 0),
        execution_receive_ns bigint,
        resolved_at_market_ns bigint,
        created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
        resolved_at timestamptz,
        PRIMARY KEY (deployment_id, decision_sequence),
        CONSTRAINT model_deployment_decisions_held_intends_no_order
            CHECK ((state = 'held') = (client_order_id IS NULL)),
        CONSTRAINT model_deployment_decisions_order_is_complete CHECK (
            (client_order_id IS NULL) = (side IS NULL) AND
            (client_order_id IS NULL) = (quantity_raw IS NULL) AND
            (client_order_id IS NULL) = (not_before_receive_ns IS NULL) AND
            (client_order_id IS NULL) = (expires_at_market_ns IS NULL)
        ),
        CONSTRAINT model_deployment_decisions_resolution_matches_state CHECK (
            (state IN ('held', 'pending')) = (resolution IS NULL) AND
            (state IN ('recorded', 'abandoned')) = (resolved_at_market_ns IS NOT NULL)
        ),
        CONSTRAINT model_deployment_decisions_execution_matches_state CHECK (
            (state = 'recorded') = (execution_ingest_sequence IS NOT NULL) AND
            (state = 'recorded') = (execution_receive_ns IS NOT NULL)
        ),
        CONSTRAINT model_deployment_decisions_reason_matches_state
            CHECK ((state = 'held') = (policy_reason <> 'order_scheduled'))
    );

    CREATE UNIQUE INDEX model_deployment_decisions_one_pending
        ON model_deployment_decisions (deployment_id)
        WHERE state = 'pending';
    CREATE UNIQUE INDEX model_deployment_decisions_client_order
        ON model_deployment_decisions (deployment_id, client_order_id)
        WHERE client_order_id IS NOT NULL;

    -- Only the owning deployment's model order for a pending decision may
    -- be journaled into a deployment's account.
    CREATE FUNCTION paper_order_respects_deployment_ownership() RETURNS trigger
        LANGUAGE plpgsql AS $function$
    DECLARE
        owner text;
    BEGIN
        SELECT deployment_id INTO owner
        FROM model_deployments
        WHERE account_id = NEW.account_id;
        IF owner IS NULL THEN
            RETURN NEW;
        END IF;
        IF NEW.source_kind <> 'model' OR NEW.source_id <> owner OR NOT EXISTS (
            SELECT 1
            FROM model_deployment_decisions
            WHERE deployment_id = owner AND
                  client_order_id = NEW.client_order_id AND
                  state = 'pending' AND
                  instrument = NEW.instrument AND
                  side = NEW.side AND quantity_raw = NEW.quantity_raw AND
                  NEW.order_type = 'market' AND NEW.time_in_force = 'ioc' AND
                  NEW.limit_price_raw IS NULL
        ) THEN
            RAISE EXCEPTION
                'account % belongs to model deployment %; only its pending decisions may record orders',
                NEW.account_id, owner
                USING ERRCODE = 'insufficient_privilege';
        END IF;
        RETURN NEW;
    END
    $function$;
    CREATE TRIGGER paper_order_records_deployment_ownership
        BEFORE INSERT ON paper_order_records
        FOR EACH ROW EXECUTE FUNCTION paper_order_respects_deployment_ownership();

    -- Matching an owner's pending ID is necessary, but it is not authority
    -- to commit through an ordinary order writer. Defer this second check
    -- until commit, after the trusted path has resolved the decision in the
    -- same transaction. A pending or abandoned decision cannot have a
    -- committed order, including when an older writer bypasses the service.
    CREATE FUNCTION paper_deployment_order_is_resolved() RETURNS trigger
        LANGUAGE plpgsql AS $function$
    DECLARE
        owner text;
    BEGIN
        SELECT deployment_id INTO owner FROM model_deployments
        WHERE account_id = NEW.account_id;
        IF owner IS NOT NULL AND NOT EXISTS (
            SELECT 1 FROM model_deployment_decisions
            WHERE deployment_id = owner AND client_order_id = NEW.client_order_id
                  AND state = 'recorded'
                  AND execution_ingest_sequence = NEW.market_ingest_sequence
                  AND execution_receive_ns = NEW.market_receive_timestamp_ns
                  AND resolved_at_market_ns = NEW.submitted_at_ns
        ) THEN
            RAISE EXCEPTION
                'account % belongs to model deployment %; its order and decision must resolve together',
                NEW.account_id, owner
                USING ERRCODE = 'check_violation';
        END IF;
        RETURN NEW;
    END
    $function$;
    CREATE CONSTRAINT TRIGGER paper_order_records_deployment_resolution
        AFTER INSERT ON paper_order_records
        DEFERRABLE INITIALLY DEFERRED
        FOR EACH ROW EXECUTE FUNCTION paper_deployment_order_is_resolved();

    INSERT INTO chronos_schema_migrations (version, description)
    VALUES (
        5,
        'model deployments: dedicated model-owned accounts, durable decisions, '
        'and the deployment ownership guard'
    );
END
$migration$;

COMMIT;
