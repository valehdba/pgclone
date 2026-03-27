/* pgx_clone--1.0.0.sql */

\echo Use "CREATE EXTENSION pgx_clone" to load this file. \quit

-- SYNCHRONOUS FUNCTIONS
CREATE FUNCTION pgx_clone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_table' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgx_clone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_table' LANGUAGE C VOLATILE;
CREATE FUNCTION pgx_clone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_table' LANGUAGE C VOLATILE;
CREATE FUNCTION pgx_clone_table_ex(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_table_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION pgx_clone_schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_schema' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgx_clone_schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_schema' LANGUAGE C VOLATILE;
CREATE FUNCTION pgx_clone_schema_ex(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_schema_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION pgx_clone_functions(source_conninfo TEXT, schema_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_functions' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgx_clone_database(source_conninfo TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_database' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgx_clone_database(source_conninfo TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_database' LANGUAGE C VOLATILE;
CREATE FUNCTION pgx_clone_version() RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_version' LANGUAGE C IMMUTABLE STRICT;

-- ASYNC FUNCTIONS (require shared_preload_libraries = 'pgx_clone')
CREATE FUNCTION pgx_clone_table_async(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true, target_table_name TEXT DEFAULT NULL, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgx_clone_table_async' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgx_clone_table_async(TEXT, TEXT, TEXT, BOOLEAN, TEXT, TEXT) IS 'Async table clone. Options: {"indexes","constraints","triggers": bool, "conflict": "error|skip|replace|rename"}. Returns job_id.';

CREATE FUNCTION pgx_clone_schema_async(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgx_clone_schema_async' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgx_clone_schema_async(TEXT, TEXT, BOOLEAN, TEXT) IS 'Async schema clone via background worker. Returns job_id.';

-- PROGRESS TRACKING
CREATE TYPE pgx_clone_progress_type AS (job_id INTEGER, status TEXT, current_object TEXT, completed_tables BIGINT, total_tables BIGINT, copied_rows BIGINT, bytes_transferred BIGINT, constraints_cloned INTEGER, indexes_cloned INTEGER, triggers_cloned INTEGER, error_message TEXT);

CREATE FUNCTION pgx_clone_progress(job_id INTEGER) RETURNS pgx_clone_progress_type AS 'MODULE_PATHNAME', 'pgx_clone_progress' LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgx_clone_progress(INTEGER) IS 'Get progress of an async clone job.';

CREATE FUNCTION pgx_clone_cancel(job_id INTEGER) RETURNS BOOLEAN AS 'MODULE_PATHNAME', 'pgx_clone_cancel' LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgx_clone_cancel(INTEGER) IS 'Cancel a running or queued clone job.';

CREATE FUNCTION pgx_clone_resume(job_id INTEGER) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgx_clone_resume' LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgx_clone_resume(INTEGER) IS 'Resume a failed/cancelled schema clone from where it stopped. Returns new job_id.';
