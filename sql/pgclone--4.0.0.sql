/* pgclone--4.0.0.sql */
\echo Use "CREATE EXTENSION pgclone" to load this file. \quit

-- v4.0.0: All functions now live under the 'pgclone' schema.
-- Usage: SELECT pgclone.table(...), pgclone.schema(...), etc.
CREATE SCHEMA IF NOT EXISTS pgclone;

-- SYNCHRONOUS
CREATE FUNCTION table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE;
CREATE FUNCTION table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION table(TEXT, TEXT, TEXT, BOOLEAN, TEXT, TEXT) IS 'Clone table with JSON options: {"columns":["col1","col2"], "where":"status=''active''", "indexes":false, "constraints":false, "triggers":false, "mask":{"email":"email","name":"name","phone":"phone","col":{"type":"partial","prefix":2,"suffix":3},"col2":"hash","col3":"null","col4":{"type":"random_int","min":0,"max":100},"col5":{"type":"constant","value":"REDACTED"}}}';
CREATE FUNCTION table_ex(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema' LANGUAGE C VOLATILE;
CREATE FUNCTION schema_ex(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION functions(source_conninfo TEXT, schema_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_functions' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION database(source_conninfo TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION database(source_conninfo TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database' LANGUAGE C VOLATILE;

-- v2.0.1: Create target database and clone into it
CREATE FUNCTION database_create(source_conninfo TEXT, target_dbname TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database_create' LANGUAGE C VOLATILE;
CREATE FUNCTION database_create(source_conninfo TEXT, target_dbname TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database_create' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION database_create(TEXT, TEXT, BOOLEAN) IS 'Create target database if not exists, then clone all schemas/tables/functions from source. Run from postgres DB.';

-- ASYNC (require shared_preload_libraries = 'pgclone')
CREATE FUNCTION table_async(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true, target_table_name TEXT DEFAULT NULL, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_table_async' LANGUAGE C VOLATILE;
CREATE FUNCTION schema_async(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_schema_async' LANGUAGE C VOLATILE;

-- PROGRESS & JOB MANAGEMENT
CREATE FUNCTION progress(job_id INTEGER) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_progress' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION cancel(job_id INTEGER) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_cancel' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION resume(job_id INTEGER) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_resume' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION jobs() RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_jobs' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION clear_jobs() RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_clear_jobs' LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION clear_jobs() IS 'Clear completed/failed/cancelled job slots from shared memory';

-- v2.1.0+v2.1.1+v2.1.2: Progress Tracking View with progress bar, elapsed time, ETA
CREATE FUNCTION progress_detail()
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

COMMENT ON FUNCTION progress_detail() IS 'Returns tabular progress with visual progress bar and elapsed time for all clone jobs';

-- VIEW: convenient wrapper
CREATE VIEW jobs_view AS
    SELECT * FROM pgclone.progress_detail();

COMMENT ON VIEW jobs_view IS 'Live progress tracking view with progress bar and elapsed time for all pgclone async clone jobs';

-- v3.1.0: Auto-discovery of sensitive data
CREATE FUNCTION discover_sensitive(source_conninfo TEXT, schema_name TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_discover_sensitive'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION discover_sensitive(TEXT, TEXT) IS 'Scan source schema for columns matching sensitive data patterns (email, name, phone, ssn, salary, etc.) and return suggested mask rules as JSON';

-- v3.2.0: Static data masking on local tables
CREATE FUNCTION mask_in_place(schema_name TEXT, table_name TEXT, mask_json TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_mask_in_place'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION mask_in_place(TEXT, TEXT, TEXT) IS 'Apply data masking to an existing local table via UPDATE. mask_json uses same format as clone mask option: {"email": "email", "name": "name", "ssn": "null"}';

-- v3.3.0: Dynamic data masking via views and role-based access
CREATE FUNCTION create_masking_policy(schema_name TEXT, table_name TEXT, mask_json TEXT, privileged_role TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_create_masking_policy'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION create_masking_policy(TEXT, TEXT, TEXT, TEXT) IS 'Create a dynamic masking policy: creates a masked view, revokes base table access from PUBLIC, grants view to PUBLIC, grants base table to privileged role';

CREATE FUNCTION drop_masking_policy(schema_name TEXT, table_name TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_drop_masking_policy'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION drop_masking_policy(TEXT, TEXT) IS 'Remove a dynamic masking policy: drops the masked view and restores base table access to PUBLIC';

-- v3.4.0: Clone roles with permissions and passwords
CREATE FUNCTION clone_roles(source_conninfo TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_clone_roles'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION clone_roles(TEXT) IS 'Clone all non-system roles from source with encrypted passwords, attributes, memberships, and all permissions. Requires superuser on both source and target.';

CREATE FUNCTION clone_roles(source_conninfo TEXT, role_names TEXT)
RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_clone_roles'
LANGUAGE C VOLATILE;
COMMENT ON FUNCTION clone_roles(TEXT, TEXT) IS 'Clone specific roles (comma-separated) from source with encrypted passwords, attributes, memberships, and permissions. If role exists on target, syncs password and attributes without dropping.';

-- v3.5.0: Clone verification — compare row counts
CREATE FUNCTION verify(source_conninfo TEXT, schema_name TEXT)
RETURNS TABLE (
    schema_name  TEXT,
    table_name   TEXT,
    source_rows  BIGINT,
    target_rows  BIGINT,
    match        TEXT
) AS 'MODULE_PATHNAME', 'pgclone_verify'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION verify(TEXT, TEXT) IS 'Compare row counts between source and local target for all tables in a schema. Returns side-by-side comparison with match status.';

CREATE FUNCTION verify(source_conninfo TEXT)
RETURNS TABLE (
    schema_name  TEXT,
    table_name   TEXT,
    source_rows  BIGINT,
    target_rows  BIGINT,
    match        TEXT
) AS 'MODULE_PATHNAME', 'pgclone_verify'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION verify(TEXT) IS 'Compare row counts between source and local target for all user tables across all schemas. Returns side-by-side comparison with match status.';

-- v3.6.0: GDPR/Compliance masking report
CREATE FUNCTION masking_report(schema_name TEXT)
RETURNS TABLE (
    schema_name    TEXT,
    table_name     TEXT,
    column_name    TEXT,
    sensitivity    TEXT,
    mask_status    TEXT,
    recommendation TEXT
) AS 'MODULE_PATHNAME', 'pgclone_masking_report'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION masking_report(TEXT) IS 'Generate GDPR/compliance audit report: lists sensitive columns, their masking status, and recommendations. Checks for masked views.';

-- VERSION
CREATE FUNCTION version() RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_version' LANGUAGE C IMMUTABLE STRICT;
