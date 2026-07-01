/* pgclone--4.4.2.sql */
\echo Use "CREATE EXTENSION pgclone" to load this file. \quit

-- v4.4.2: Constraint- and length-aware data masking (GitHub issue #18).
--   No new SQL surface — the fix is entirely in the C masking engine.
--   A mask is now skipped (with a WARNING) when applying it would break
--   the clone: text/constant/partial values that overflow a varchar(N)
--   column are clamped to fit; a "constant" that is not valid for a
--   numeric column, a "null" on a NOT NULL column, a value-collapsing
--   mask on a UNIQUE/PRIMARY KEY column (only "hash" is kept), and any
--   mask on a FOREIGN KEY column are all skipped. discover_sensitive()
--   and masking_report() only suggest strategies the engine will apply
--   (UNIQUE/PK and NOT NULL sensitive columns are steered to "hash").

-- v4.4.1: Type-aware data masking (GitHub issue #17).
--   No new SQL surface — the fix is entirely in the C masking engine.
--   A mask strategy whose output value the target column cannot store
--   (e.g. the numeric "random_int" strategy on a BOOLEAN column matched
--   by the %income% pattern) is now skipped with a WARNING instead of
--   aborting the clone with "invalid input syntax for type boolean".
--   pgclone.discover_sensitive() likewise no longer suggests a mask that
--   is incompatible with a column's type.

-- v4.4.0: Schema/database-level in-line masking and table subset filters
--   No new SQL surface — both features are new keys in the existing JSON
--   options argument of pgclone.schema() / pgclone.database() /
--   pgclone.database_create():
--     '{"tables": ["order_.*"], "exclude_tables": [".*_old"],
--       "masks": {"users": {"email": "email", "ssn": "null"}}}'

-- v4.3.0: Consistent-snapshot clones (REPEATABLE READ READ ONLY)
--   No new SQL surface — the behaviour is enabled by default for every
--   table/schema/database clone (sync and async, including parallel
--   pool mode) and can be disabled per-call with the options JSON:
--     SELECT pgclone.schema('...', 'public', true, '{"consistent": false}');
--   See CHANGELOG.md / docs/USAGE.md for details and tradeoffs.

-- v4.0.0: All functions now live under the 'pgclone' schema.
-- Usage: SELECT pgclone.table(...), pgclone.schema(...), etc.
CREATE SCHEMA IF NOT EXISTS pgclone;

-- SYNCHRONOUS
CREATE FUNCTION pgclone.table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone.table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone.table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgclone.table(TEXT, TEXT, TEXT, BOOLEAN, TEXT, TEXT) IS 'Clone table with JSON options: {"columns":["col1","col2"], "where":"status=''active''", "indexes":false, "constraints":false, "triggers":false, "mask":{"email":"email","name":"name","phone":"phone","col":{"type":"partial","prefix":2,"suffix":3},"col2":"hash","col3":"null","col4":{"type":"random_int","min":0,"max":100},"col5":{"type":"constant","value":"REDACTED"}}}';
CREATE FUNCTION pgclone.table_ex(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone.schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone.schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgclone.schema(TEXT, TEXT, BOOLEAN, TEXT) IS 'Clone schema with JSON options: {"indexes":false, "constraints":false, "triggers":false, "tables":["regex1","regex2"], "exclude_tables":["regex"], "masks":{"table_name":{"col":"email"},"schema.table":{"col":"hash"}}} — tables/exclude_tables are anchored POSIX regexes selecting a subset of tables; masks applies per-table in-line masking during the COPY stream (v4.4.0).';
CREATE FUNCTION pgclone.schema_ex(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone.functions(source_conninfo TEXT, schema_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_functions' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone.database(source_conninfo TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone.database(source_conninfo TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgclone.database(TEXT, BOOLEAN, TEXT) IS 'Clone database with JSON options — supports the same "tables", "exclude_tables", and "masks" options as pgclone.schema (v4.4.0); masks keys may be schema-qualified ("schema.table") to disambiguate.';

-- v2.0.1: Create target database and clone into it
CREATE FUNCTION pgclone.database_create(source_conninfo TEXT, target_dbname TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database_create' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone.database_create(source_conninfo TEXT, target_dbname TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database_create' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgclone.database_create(TEXT, TEXT, BOOLEAN) IS 'Create target database if not exists, then clone all schemas/tables/functions from source. Run from postgres DB.';

-- ASYNC (require shared_preload_libraries = 'pgclone')
CREATE FUNCTION pgclone.table_async(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true, target_table_name TEXT DEFAULT NULL, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_table_async' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone.schema_async(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_schema_async' LANGUAGE C VOLATILE;

-- PROGRESS & JOB MANAGEMENT
CREATE FUNCTION pgclone.progress(job_id INTEGER) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_progress' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone.cancel(job_id INTEGER) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_cancel' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone.resume(job_id INTEGER) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_resume' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone.jobs() RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_jobs' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone.clear_jobs() RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_clear_jobs' LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgclone.clear_jobs() IS 'Clear completed/failed/cancelled job slots from shared memory';

-- v2.1.0+v2.1.1+v2.1.2: Progress Tracking View with progress bar, elapsed time, ETA
CREATE FUNCTION pgclone.progress_detail()
RETURNS TABLE (
    job_id              INTEGER,
    status              TEXT,
    op_type             TEXT,
    schema_name         TEXT,
    table_name          TEXT,
    current_phase       TEXT,
    current_table       TEXT,
    tables_total        BIGINT,
    tables_completed    BIGINT,
    rows_copied         BIGINT,
    bytes_copied        BIGINT,
    elapsed_ms          BIGINT,
    start_time          TIMESTAMPTZ,
    end_time            TIMESTAMPTZ,
    error_message       TEXT,
    pct_complete        DOUBLE PRECISION,
    progress_bar        TEXT,
    elapsed_time        TEXT
) AS 'MODULE_PATHNAME', 'pgclone_progress_view'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pgclone.progress_detail() IS 'Returns tabular progress with visual progress bar and elapsed time for all clone jobs';

-- VIEW: convenient wrapper
CREATE VIEW pgclone.jobs_view AS
    SELECT * FROM pgclone.progress_detail();

COMMENT ON VIEW pgclone.jobs_view IS 'Live progress tracking view with progress bar and elapsed time for all pgclone async clone jobs';

-- v3.1.0: Auto-discovery of sensitive data
CREATE FUNCTION pgclone.discover_sensitive(source_conninfo TEXT, schema_name TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_discover_sensitive'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgclone.discover_sensitive(TEXT, TEXT) IS 'Scan source schema for columns matching sensitive data patterns (email, name, phone, ssn, salary, etc.) and return suggested mask rules as JSON';

-- v3.2.0: Static data masking on local tables
CREATE FUNCTION pgclone.mask_in_place(schema_name TEXT, table_name TEXT, mask_json TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_mask_in_place'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgclone.mask_in_place(TEXT, TEXT, TEXT) IS 'Apply data masking to an existing local table via UPDATE. mask_json uses same format as clone mask option: {"email": "email", "name": "name", "ssn": "null"}';

-- v3.3.0: Dynamic data masking via views and role-based access
CREATE FUNCTION pgclone.create_masking_policy(schema_name TEXT, table_name TEXT, mask_json TEXT, privileged_role TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_create_masking_policy'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgclone.create_masking_policy(TEXT, TEXT, TEXT, TEXT) IS 'Create a dynamic masking policy: creates a masked view, revokes base table access from PUBLIC, grants view to PUBLIC, grants base table to privileged role';

CREATE FUNCTION pgclone.drop_masking_policy(schema_name TEXT, table_name TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_drop_masking_policy'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgclone.drop_masking_policy(TEXT, TEXT) IS 'Remove a dynamic masking policy: drops the masked view and restores base table access to PUBLIC';

-- v3.4.0: Clone roles with permissions and passwords
CREATE FUNCTION pgclone.clone_roles(source_conninfo TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_clone_roles'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgclone.clone_roles(TEXT) IS 'Clone all non-system roles from source with encrypted passwords, attributes, memberships, and all permissions. Requires superuser on both source and target.';

CREATE FUNCTION pgclone.clone_roles(source_conninfo TEXT, role_names TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_clone_roles'
LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgclone.clone_roles(TEXT, TEXT) IS 'Clone specific roles (comma-separated) from source with encrypted passwords, attributes, memberships, and permissions. If role exists on target, syncs password and attributes without dropping.';

-- v3.5.0: Clone verification — compare row counts
CREATE FUNCTION pgclone.verify(source_conninfo TEXT, schema_name TEXT)
RETURNS TABLE (
    schema_name  TEXT,
    table_name   TEXT,
    source_rows  BIGINT,
    target_rows  BIGINT,
    match        TEXT
) AS 'MODULE_PATHNAME', 'pgclone_verify'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgclone.verify(TEXT, TEXT) IS 'Compare row counts between source and local target for all tables in a schema. Returns side-by-side comparison with match status.';

CREATE FUNCTION pgclone.verify(source_conninfo TEXT)
RETURNS TABLE (
    schema_name  TEXT,
    table_name   TEXT,
    source_rows  BIGINT,
    target_rows  BIGINT,
    match        TEXT
) AS 'MODULE_PATHNAME', 'pgclone_verify'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgclone.verify(TEXT) IS 'Compare row counts between source and local target for all user tables across all schemas. Returns side-by-side comparison with match status.';

-- v3.6.0: GDPR/Compliance masking report
CREATE FUNCTION pgclone.masking_report(schema_name TEXT)
RETURNS TABLE (
    schema_name    TEXT,
    table_name     TEXT,
    column_name    TEXT,
    sensitivity    TEXT,
    mask_status    TEXT,
    recommendation TEXT
) AS 'MODULE_PATHNAME', 'pgclone_masking_report'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgclone.masking_report(TEXT) IS 'Generate GDPR/compliance audit report: lists sensitive columns, their masking status, and recommendations. Checks for masked views.';

-- v4.1.0: Schema diff — DDL drift detection between source and local target
CREATE FUNCTION pgclone.diff(source_conninfo TEXT, schema_name TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_diff'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgclone.diff(TEXT, TEXT) IS
    'Compare DDL of a schema between source and the local target. '
    'Returns JSON drift report listing objects only_in_source / only_in_target / modified '
    'across tables (with per-column type/nullability/default drift), indexes, '
    'constraints, triggers, views, and sequences. Read-only on both sides.';

-- v4.2.0: Pre-flight validator — connection, permissions, version, capacity,
--         name-conflict, role and tablespace checks before a clone.
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

-- VERSION
CREATE FUNCTION pgclone.version() RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_version' LANGUAGE C IMMUTABLE STRICT;
