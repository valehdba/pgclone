/* pgclone--4.3.1--4.3.2.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.3.2: Snapshot-keeper resilience for background workers (issue #9, bgw mirror)
--
-- v4.3.1 fixed the synchronous path. v4.3.2 ports the same four-layer
-- fix to src/pgclone_bgw.c so the async paths (pgclone.table_async,
-- pgclone.schema_async sequential, pgclone.schema_async parallel pool)
-- no longer fail with "invalid snapshot identifier" on networked
-- source connections.
--
-- No SQL signature changes. The fix is entirely in the C module:
--   * bgw_connect_with_keepalives() — new helper mirroring
--     pgclone_connect_with_keepalives() in pgclone.c. Replaces three
--     direct PQconnectdb() call sites (single-job worker, pool
--     worker, pool coordinator). Injects keepalives=1
--     keepalives_idle=30 keepalives_interval=10 keepalives_count=6
--     only when the user did not set them.
--   * bgw_begin_repeatable_read() now issues SET LOCAL
--     idle_in_transaction_session_timeout = 0 and SET LOCAL
--     statement_timeout = 0 immediately after BEGIN, defeating
--     non-zero source-side timeouts on the keeper transaction.
--   * bgw_begin_with_imported_snapshot() emits a clearer WARNING
--     with diagnostic context when SET TRANSACTION SNAPSHOT fails
--     with "invalid snapshot identifier".
--   * bgw_keeper_ping() — new helper. Called every ~5 s inside the
--     pool coordinator's wait loop to surface a silently-dropped
--     keeper connection before pool workers fail to import.
