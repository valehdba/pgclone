/* pgclone--4.2.0--4.3.0.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.3.0: Consistent-snapshot clones (REPEATABLE READ READ ONLY)
--
-- This release is a behaviour change with no SQL surface change:
--   - Every source-side read now happens inside a
--     `BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY` transaction.
--   - Multi-connection operations (schema clone, database clone,
--     parallel pool mode) share one snapshot via
--     `pg_export_snapshot()` / `SET TRANSACTION SNAPSHOT`, so every
--     COPY across every connection / worker reads the same point-in-
--     time view of the source.
--   - Cross-table FK consistency is now guaranteed against a live
--     source — the same correctness model as `pg_dump -j`.
--
-- Opt-out per call (e.g. when long source snapshots are undesirable):
--   SELECT pgclone.schema('...', 'public', true, '{"consistent": false}');
--   SELECT pgclone.database('...', true, '{"consistent": false}');
--   SELECT pgclone.schema_async('...', 'public', true,
--                                '{"parallel": 4, "consistent": false}');
--
-- See CHANGELOG.md and docs/USAGE.md for details.
