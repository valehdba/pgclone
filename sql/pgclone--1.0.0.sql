/* pgclone--1.0.0.sql */
\echo Use "CREATE EXTENSION pgclone" to load this file. \quit

CREATE FUNCTION pgclone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_table(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_table_ex(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN, target_table_name TEXT, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_schema(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_schema_ex(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN, include_indexes BOOLEAN DEFAULT true, include_constraints BOOLEAN DEFAULT true, include_triggers BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_schema_ex' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_functions(source_conninfo TEXT, schema_name TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_functions' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_database(source_conninfo TEXT, include_data BOOLEAN DEFAULT true) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_database(source_conninfo TEXT, include_data BOOLEAN, options TEXT) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_database' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_table_async(source_conninfo TEXT, schema_name TEXT, table_name TEXT, include_data BOOLEAN DEFAULT true, target_table_name TEXT DEFAULT NULL, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_table_async' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_schema_async(source_conninfo TEXT, schema_name TEXT, include_data BOOLEAN DEFAULT true, options TEXT DEFAULT NULL) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_schema_async' LANGUAGE C VOLATILE;
CREATE FUNCTION pgclone_progress(job_id INTEGER) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_progress' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_cancel(job_id INTEGER) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_cancel' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_resume(job_id INTEGER) RETURNS INTEGER AS 'MODULE_PATHNAME', 'pgclone_resume' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_jobs() RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_jobs' LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION pgclone_version() RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_version' LANGUAGE C IMMUTABLE STRICT;
