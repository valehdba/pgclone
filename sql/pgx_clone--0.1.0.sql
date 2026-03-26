/* pgx_clone--0.1.0.sql
 *
 * SQL definitions for pgx_clone extension v0.1.0
 */

-- Complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgx_clone" to load this file. \quit

-- ============================================================
-- Clone a single table (structure + optional data)
-- ============================================================
CREATE FUNCTION pgx_clone_table(
    source_conninfo TEXT,
    schema_name     TEXT,
    table_name      TEXT,
    include_data    BOOLEAN DEFAULT true
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_table'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pgx_clone_table(TEXT, TEXT, TEXT, BOOLEAN) IS
'Clone a table from a remote PostgreSQL host. Set include_data=false for structure only.';

-- ============================================================
-- Clone an entire schema (tables, views, functions, sequences)
-- ============================================================
CREATE FUNCTION pgx_clone_schema(
    source_conninfo TEXT,
    schema_name     TEXT,
    include_data    BOOLEAN DEFAULT true
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_schema'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pgx_clone_schema(TEXT, TEXT, BOOLEAN) IS
'Clone an entire schema including tables, views, functions, and sequences.';

-- ============================================================
-- Clone all functions/procedures from a schema
-- ============================================================
CREATE FUNCTION pgx_clone_functions(
    source_conninfo TEXT,
    schema_name     TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_functions'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pgx_clone_functions(TEXT, TEXT) IS
'Clone all functions and procedures from a remote schema.';

-- ============================================================
-- Clone an entire database (all user schemas)
-- ============================================================
CREATE FUNCTION pgx_clone_database(
    source_conninfo TEXT,
    include_data    BOOLEAN DEFAULT true
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_database'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pgx_clone_database(TEXT, BOOLEAN) IS
'Clone all user schemas from a remote database. Excludes system schemas.';

-- ============================================================
-- Version info
-- ============================================================
CREATE FUNCTION pgx_clone_version()
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_version'
LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION pgx_clone_version() IS
'Returns the pgx_clone extension version.';
