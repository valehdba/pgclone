/* pgclone--4.8.0--4.9.0.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.9.0: no SQL-level changes. This release is C-level only:
--   * "on_error": "continue" option for pgclone.schema() / pgclone.database()
-- Restart PostgreSQL after installing the new library (shared_preload_libraries).
