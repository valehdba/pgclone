/* pgclone--0.2.0.sql
 *
 * SQL definitions for pgclone extension v0.2.0
 */

-- Complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgclone" to load this file. \quit

-- ============================================================
-- Clone a single table (structure + optional data)
-- Indexes, constraints, triggers: all ON by default
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
'Clone a table from a remote host. Includes indexes, constraints, and triggers.';

-- ============================================================
-- Clone a table with a different name on target
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
'Clone a table with a different name on target.';

-- ============================================================
-- Clone a table with JSON options to control indexes/constraints/triggers
-- Options: {"indexes": bool, "constraints": bool, "triggers": bool}
-- ============================================================
CREATE FUNCTION pgclone_table(
    source_conninfo   TEXT,
    schema_name       TEXT,
    table_name        TEXT,
    include_data      BOOLEAN,
    target_table_name TEXT,
    options           TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgclone_table'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgclone_table(TEXT, TEXT, TEXT, BOOLEAN, TEXT, TEXT) IS
'Clone a table with JSON options: {"indexes": false, "constraints": false, "triggers": false}';

-- ============================================================
-- Clone a table with separate boolean controls
-- ============================================================
CREATE FUNCTION pgclone_table_ex(
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
AS 'MODULE_PATHNAME', 'pgclone_table_ex'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgclone_table_ex(TEXT, TEXT, TEXT, BOOLEAN, TEXT, BOOLEAN, BOOLEAN, BOOLEAN) IS
'Clone a table with separate boolean controls for indexes, constraints, and triggers.';

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
'Clone an entire schema. Includes indexes, constraints, and triggers.';

-- ============================================================
-- Clone schema with JSON options
-- ============================================================
CREATE FUNCTION pgclone_schema(
    source_conninfo TEXT,
    schema_name     TEXT,
    include_data    BOOLEAN,
    options         TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgclone_schema'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgclone_schema(TEXT, TEXT, BOOLEAN, TEXT) IS
'Clone a schema with JSON options: {"indexes": false, "constraints": false, "triggers": false}';

-- ============================================================
-- Clone schema with separate boolean controls
-- ============================================================
CREATE FUNCTION pgclone_schema_ex(
    source_conninfo      TEXT,
    schema_name          TEXT,
    include_data         BOOLEAN,
    include_indexes      BOOLEAN DEFAULT true,
    include_constraints  BOOLEAN DEFAULT true,
    include_triggers     BOOLEAN DEFAULT true
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgclone_schema_ex'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgclone_schema_ex(TEXT, TEXT, BOOLEAN, BOOLEAN, BOOLEAN, BOOLEAN) IS
'Clone a schema with separate boolean controls for indexes, constraints, and triggers.';

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
'Clone all user schemas from a remote database.';

-- ============================================================
-- Clone database with JSON options
-- ============================================================
CREATE FUNCTION pgclone_database(
    source_conninfo TEXT,
    include_data    BOOLEAN,
    options         TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgclone_database'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pgclone_database(TEXT, BOOLEAN, TEXT) IS
'Clone a database with JSON options: {"indexes": false, "constraints": false, "triggers": false}';

-- ============================================================
-- Version info
-- ============================================================
CREATE FUNCTION pgclone_version()
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pgclone_version'
LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION pgclone_version() IS
'Returns the pgclone extension version.';
