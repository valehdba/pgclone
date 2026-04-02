/* pgclone--2.0.1.sql */
\echo Use "CREATE EXTENSION pgclone" to load this file. \quit

-- SYNCHRONOUS
CREATE FUNCTION pgclone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgclone_table(TEXT, TEXT, TEXT, BOOLEAN, TEXT, TEXT) IS 'Clone table with JSON options: {"columns":["col1","col2"], "where":"status=''active''", "indexes":false, "constraints":false, "triggers":false}';
CREATE FUNCTION pgclone_table_ex(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_schema_ex(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_functions(source_conninfo TEXT, schema_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_functions' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_database(source_conninfo TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_database(source_conninfo TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database' LANGUAGE C VOLATILE;

-- NEW in v2.0.1: Create target database and clone into it
CREATE FUNCTION pgclone_database_create(source_conninfo TEXT, target_dbname TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database_create' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_database_create(source_conninfo TEXT, target_dbname TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database_create' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgclone_database_create(TEXT, TEXT, BOOLEAN) IS 'Create target database if not exists, then clone all schemas/tables/functions from source. Run from postgres DB.';

-- ASYNC (require shared_preload_libraries = 'pgclone')
CREATE FUNCTION pgclone_table_async(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true, target_table_name TEXT DEFAULT NULL, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_table_async' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_schema_async(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_schema_async' LANGUAGE C VOLATILE;

-- PROGRESS & JOB MANAGEMENT
CREATE FUNCTION pgclone_progress(job_id INTEGER) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_progress' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_cancel(job_id INTEGER) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_cancel' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_resume(job_id INTEGER) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_resume' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_jobs() RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_jobs' LANGUAGE C VOLATILE STRICT;

-- VERSION
CREATE FUNCTION pgclone_version() RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_version' LANGUAGE C IMMUTABLE STRICT;
