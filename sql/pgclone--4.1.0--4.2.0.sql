/* pgclone--4.1.0--4.2.0.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.2.0: Pre-flight validator — connection, permissions, version,
--         capacity, name-conflict, role and tablespace checks before
--         a clone operation. Read-only on both sides; never executes
--         DDL or DML.

CREATE FUNCTION pgclone.preflight(source_conninfo TEXT, schema_name TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_preflight'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pgclone.preflight(TEXT, TEXT) IS
    'Validate that a clone of the given schema from source into the local '
    'target is likely to succeed. Returns JSON with errors / warnings / info '
    'arrays plus a per-check breakdown covering: source/target connection, '
    'PostgreSQL versions, schema existence, USAGE/SELECT/CREATE permissions, '
    'estimated source size, target database size, object counts, name '
    'conflicts on the target schema, missing extensions, missing roles, and '
    'missing non-default tablespaces. Read-only on both sides.';
