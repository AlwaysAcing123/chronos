-- Paper deployments of registered model versions (ADR 0021).
--
--   model_deployments.model_reference
--       experiment: an experiment's fitted model, as since migration 005,
--       named by experiment_id and model_name (logistic or majority).
--       registered: one immutable registered model version, named by
--       model_version_id; it has no experiment, and model_name is the
--       version's family (logistic, majority, or onnx).
--   model_deployments.model_version_id
--   model_deployments.training_cutoff_basis
--       verified: Chronos verified the training cutoff (every experiment
--       model, and a registered version taken from one). declared: an
--       import's uploader declared it; Chronos did not verify it.
--
-- Migration 005 required an experiment ID and a logistic or majority model
-- of every deployment. Those two column constraints become one constraint
-- per variant, and the experiment variant keeps exactly 005's rules: an
-- experiment ID, no version ID, a logistic or majority model, and a
-- verified cutoff. A registered deployment must name a version and no
-- experiment. Every existing row is an experiment deployment and satisfies
-- this. The decision table, the one-account-per-deployment constraint, and
-- the order trigger are not touched.
--
-- The defaults keep a server from before this migration creating only
-- experiment deployments, correctly. The file owns its transaction and is
-- idempotent: once version 8 is recorded it does nothing.
BEGIN;

DO $migration$
BEGIN
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 8) THEN
        RETURN;
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM chronos_schema_migrations WHERE version = 7
    ) THEN
        RAISE EXCEPTION 'migration 008 requires migration 007'
            USING ERRCODE = 'object_not_in_prerequisite_state';
    END IF;
    -- As migration 005: paper_accounts first, as every deployment writer
    -- locks it first, then the migration table to serialize a rerun.
    LOCK TABLE paper_accounts IN SHARE ROW EXCLUSIVE MODE;
    LOCK TABLE chronos_schema_migrations IN SHARE ROW EXCLUSIVE MODE;
    IF EXISTS (SELECT 1 FROM chronos_schema_migrations WHERE version = 8) THEN
        RETURN;
    END IF;

    ALTER TABLE model_deployments
        ADD COLUMN model_reference text NOT NULL DEFAULT 'experiment'
            CHECK (model_reference IN ('experiment', 'registered')),
        ADD COLUMN model_version_id text
            CHECK (model_version_id ~ '^[0-9a-f]{64}$'),
        ADD COLUMN training_cutoff_basis text NOT NULL DEFAULT 'verified'
            CHECK (training_cutoff_basis IN ('verified', 'declared'));
    ALTER TABLE model_deployments ALTER COLUMN experiment_id DROP NOT NULL;
    ALTER TABLE model_deployments DROP CONSTRAINT model_deployments_model_name_check;
    ALTER TABLE model_deployments
        ADD CONSTRAINT model_deployments_model_name_check
            CHECK (model_name IN ('logistic', 'majority', 'onnx')),
        ADD CONSTRAINT model_deployments_one_model_reference CHECK (
            (
                model_reference = 'experiment'
                AND experiment_id IS NOT NULL
                AND model_version_id IS NULL
                AND model_name IN ('logistic', 'majority')
                AND training_cutoff_basis = 'verified'
            ) OR (
                model_reference = 'registered'
                AND experiment_id IS NULL
                AND model_version_id IS NOT NULL
            )
        );

    INSERT INTO chronos_schema_migrations (version, description)
    VALUES (
        8,
        'registered model deployments: an explicit experiment or registered '
        'model reference per deployment, and its cutoff''s basis'
    );
END
$migration$;

COMMIT;
