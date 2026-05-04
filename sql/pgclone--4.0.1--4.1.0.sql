/* pgclone--4.0.1--4.1.0.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.1.0: Schema diff — DDL drift detection between source and target.
--
-- Adds pgclone.diff(source_conninfo, schema_name): a read-only
-- comparison of catalog metadata across source and local target.
-- Returns a JSON document describing tables/indexes/constraints/
-- triggers/views/sequences that exist on only one side or differ.

CREATE FUNCTION pgclone.diff(source_conninfo TEXT, schema_name TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_diff'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pgclone.diff(TEXT, TEXT) IS
    'Compare DDL of a schema between source and the local target. '
    'Returns JSON drift report listing objects only_in_source / only_in_target / modified '
    'across tables (with per-column type/nullability/default drift), indexes, '
    'constraints, triggers, views, and sequences. Read-only on both sides.';
