/* pgclone--4.3.0--4.3.1.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.3.1: Snapshot-keeper resilience (issue #9)
--
-- No SQL signature changes. The fix is entirely in the C module:
--   * TCP keepalives injected into every source connection so the
--     idle keeper survives firewall/NAT idle timeouts.
--   * SET LOCAL idle_in_transaction_session_timeout = 0 and
--     statement_timeout = 0 inside the keeper transaction so a
--     non-zero source-side setting can't kill it.
--   * Specific errhint when SET TRANSACTION SNAPSHOT returns
--     "invalid snapshot identifier" pointing at the most common
--     cause and the {"consistent": false} opt-out.
--   * Keeper liveness ping between sub-phases of pgclone.schema()
--     and pgclone.database() so a silent connection drop produces
--     a clear error before — not during — the next importer.
