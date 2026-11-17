/* pgclone--4.7.0--4.8.0.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.8.0: no SQL-level changes. This release is C-level only:
--   * "conflict": "ignore" option
--   * partitioned tables cloned with PARTITION BY / PARTITION OF
--   * generated columns stay generated
--   * table_async creates and syncs sequences
--   * background workers honor SIGTERM on PG17+
-- Restart PostgreSQL after installing the new library (shared_preload_libraries).
