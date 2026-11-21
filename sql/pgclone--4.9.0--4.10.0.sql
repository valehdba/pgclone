/* pgclone--4.9.0--4.10.0.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.10.0: no SQL-level changes. This release is C-level only:
--   * PostgreSQL 19 support (get_database_name() header move)
-- Restart PostgreSQL after installing the new library (shared_preload_libraries).
