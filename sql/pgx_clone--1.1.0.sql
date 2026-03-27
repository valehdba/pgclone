/* pgx_clone--1.1.0.sql */
\echo Use "CREATE EXTENSION pgx_clone" to load this file. \quit

-- SYNCHRONOUS
CREATE FUNCTION pgx_clone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_table' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgx_clone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_table' LANGUAGE C VOLATILE;
CREATE FUNCTION pgx_clone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_table' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgx_clone_table(TEXT, TEXT, TEXT, BOOLEAN, TEXT, TEXT) IS 'Clone table with JSON options: {"columns":["col1","col2"], "where":"status=''active''", "indexes":false, "constraints":false, "triggers":false}';
CREATE FUNCTION pgx_clone_table_ex(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_table_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION pgx_clone_schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_schema' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgx_clone_schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_schema' LANGUAGE C VOLATILE;
CREATE FUNCTION pgx_clone_schema_ex(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_schema_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION pgx_clone_functions(source_conninfo TEXT, schema_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_functions' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgx_clone_database(source_conninfo TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_database' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgx_clone_database(source_conninfo TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_database' LANGUAGE C VOLATILE;

-- ASYNC (require shared_preload_libraries = 'pgx_clone')
CREATE FUNCTION pgx_clone_table_async(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true, target_table_name TEXT DEFAULT NULL, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgx_clone_table_async' LANGUAGE C VOLATILE;
CREATE FUNCTION pgx_clone_schema_async(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgx_clone_schema_async' LANGUAGE C VOLATILE;

-- PROGRESS & JOB MANAGEMENT
CREATE FUNCTION pgx_clone_progress(job_id INTEGER) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_progress' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgx_clone_cancel(job_id INTEGER) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_cancel' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgx_clone_resume(job_id INTEGER) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgx_clone_resume' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgx_clone_jobs() RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_jobs' LANGUAGE C VOLATILE STRICT;

-- VERSION
CREATE FUNCTION pgx_clone_version() RETURNS TEXT AS 'MODULE_PATHNAME', 'pgx_clone_version' LANGUAGE C IMMUTABLE STRICT;
