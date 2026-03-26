/* pgx_clone--0.2.0.sql
 *
 * SQL definitions for pgx_clone extension v0.2.0
 */

-- Complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgx_clone" to load this file. \quit

-- ============================================================
-- Clone a single table (structure + optional data)
-- Indexes, constraints, triggers: all ON by default
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
'Clone a table from a remote host. Includes indexes, constraints, and triggers.';

-- ============================================================
-- Clone a table with a different name on target
-- ============================================================
CREATE FUNCTION pgx_clone_table(
    source_conninfo   TEXT,
    schema_name       TEXT,
    table_name        TEXT,
    include_data      BOOLEAN,
    target_table_name TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_table'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgx_clone_table(TEXT, TEXT, TEXT, BOOLEAN, TEXT) IS
'Clone a table with a different name on target.';

-- ============================================================
-- Clone a table with JSON options to control indexes/constraints/triggers
-- Options: {"indexes": bool, "constraints": bool, "triggers": bool}
-- ============================================================
CREATE FUNCTION pgx_clone_table(
    source_conninfo   TEXT,
    schema_name       TEXT,
    table_name        TEXT,
    include_data      BOOLEAN,
    target_table_name TEXT,
    options           TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_table'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgx_clone_table(TEXT, TEXT, TEXT, BOOLEAN, TEXT, TEXT) IS
'Clone a table with JSON options: {"indexes": false, "constraints": false, "triggers": false}';

-- ============================================================
-- Clone a table with separate boolean controls
-- ============================================================
CREATE FUNCTION pgx_clone_table_ex(
    source_conninfo      TEXT,
    schema_name          TEXT,
    table_name           TEXT,
    include_data         BOOLEAN,
    target_table_name    TEXT,
    include_indexes      BOOLEAN DEFAULT true,
    include_constraints  BOOLEAN DEFAULT true,
    include_triggers     BOOLEAN DEFAULT true
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_table_ex'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgx_clone_table_ex(TEXT, TEXT, TEXT, BOOLEAN, TEXT, BOOLEAN, BOOLEAN, BOOLEAN) IS
'Clone a table with separate boolean controls for indexes, constraints, and triggers.';

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
'Clone an entire schema. Includes indexes, constraints, and triggers.';

-- ============================================================
-- Clone schema with JSON options
-- ============================================================
CREATE FUNCTION pgx_clone_schema(
    source_conninfo TEXT,
    schema_name     TEXT,
    include_data    BOOLEAN,
    options         TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_schema'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgx_clone_schema(TEXT, TEXT, BOOLEAN, TEXT) IS
'Clone a schema with JSON options: {"indexes": false, "constraints": false, "triggers": false}';

-- ============================================================
-- Clone schema with separate boolean controls
-- ============================================================
CREATE FUNCTION pgx_clone_schema_ex(
    source_conninfo      TEXT,
    schema_name          TEXT,
    include_data         BOOLEAN,
    include_indexes      BOOLEAN DEFAULT true,
    include_constraints  BOOLEAN DEFAULT true,
    include_triggers     BOOLEAN DEFAULT true
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_schema_ex'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgx_clone_schema_ex(TEXT, TEXT, BOOLEAN, BOOLEAN, BOOLEAN, BOOLEAN) IS
'Clone a schema with separate boolean controls for indexes, constraints, and triggers.';

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
'Clone all user schemas from a remote database.';

-- ============================================================
-- Clone database with JSON options
-- ============================================================
CREATE FUNCTION pgx_clone_database(
    source_conninfo TEXT,
    include_data    BOOLEAN,
    options         TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_database'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgx_clone_database(TEXT, BOOLEAN, TEXT) IS
'Clone a database with JSON options: {"indexes": false, "constraints": false, "triggers": false}';

-- ============================================================
-- Version info
-- ============================================================
CREATE FUNCTION pgx_clone_version()
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgx_clone_version'
LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION pgx_clone_version() IS
'Returns the pgx_clone extension version.';
