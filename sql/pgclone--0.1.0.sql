/* pgclone--0.1.0.sql
 *
 * SQL definitions for pgclone extension v0.1.0
 */

-- Complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgclone" to load this file. \quit

-- ============================================================
-- Clone a single table (structure + optional data)
-- ============================================================
CREATE FUNCTION pgclone_table(
    source_conninfo TEXT,
    schema_name     TEXT,
    table_name      TEXT,
    include_data    BOOLEAN DEFAULT true
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgclone_table'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pgclone_table(TEXT, TEXT, TEXT, BOOLEAN) IS
'Clone a table from a remote PostgreSQL host. Set include_data=false for structure only.';

-- ============================================================
-- Clone a single table with a different name on target
-- ============================================================
CREATE FUNCTION pgclone_table(
    source_conninfo   TEXT,
    schema_name       TEXT,
    table_name        TEXT,
    include_data      BOOLEAN,
    target_table_name TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgclone_table'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgclone_table(TEXT, TEXT, TEXT, BOOLEAN, TEXT) IS
'Clone a table from a remote host with a different name on target. Set include_data=false for structure only.';

-- ============================================================
-- Clone an entire schema (tables, views, functions, sequences)
-- ============================================================
CREATE FUNCTION pgclone_schema(
    source_conninfo TEXT,
    schema_name     TEXT,
    include_data    BOOLEAN DEFAULT true
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgclone_schema'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pgclone_schema(TEXT, TEXT, BOOLEAN) IS
'Clone an entire schema including tables, views, functions, and sequences.';

-- ============================================================
-- Clone all functions/procedures from a schema
-- ============================================================
CREATE FUNCTION pgclone_functions(
    source_conninfo TEXT,
    schema_name     TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgclone_functions'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pgclone_functions(TEXT, TEXT) IS
'Clone all functions and procedures from a remote schema.';

-- ============================================================
-- Clone an entire database (all user schemas)
-- ============================================================
CREATE FUNCTION pgclone_database(
    source_conninfo TEXT,
    include_data    BOOLEAN DEFAULT true
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgclone_database'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pgclone_database(TEXT, BOOLEAN) IS
'Clone all user schemas from a remote database. Excludes system schemas.';

-- ============================================================
-- Version info
-- ============================================================
CREATE FUNCTION pgclone_version()
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgclone_version'
LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION pgclone_version() IS
'Returns the pgclone extension version.';
