/* pgclone--4.3.2--4.4.0.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.4.0: Schema/database-level in-line masking and table subset
-- filters (GitHub discussion #16).
--
-- No SQL signature changes — both features are new keys in the
-- existing JSON options argument of pgclone.schema(),
-- pgclone.database(), and pgclone.database_create():
--
--   * "masks": {"table": {<mask obj>}, "schema.table": {<mask obj>}}
--     Per-table masking applied IN-LINE while data streams from
--     source to target (same query-based COPY pipeline as the
--     single-table "mask" option — no post-clone UPDATE, no views).
--     Schema-qualified keys win over bare table names so database
--     clones can disambiguate identically named tables.
--
--   * "tables": ["regex", ...] / "exclude_tables": ["regex", ...]
--     Clone only a subset of tables. Entries are POSIX regexes
--     anchored as ^(pattern)$ and evaluated by the source server
--     against pg_tables.tablename. Excludes are applied after
--     includes. Sequences, views, matviews, and functions of the
--     schema are still cloned in full.
--
-- Synchronous paths only: async jobs (pgclone.schema_async /
-- pgclone.database_async) do not carry the JSON options through
-- shared memory and ignore these keys, as they already do for
-- "mask".

COMMENT ON FUNCTION pgclone.schema(TEXT, TEXT, BOOLEAN, TEXT) IS 'Clone schema with JSON options: {"indexes":false, "constraints":false, "triggers":false, "tables":["regex1","regex2"], "exclude_tables":["regex"], "masks":{"table_name":{"col":"email"},"schema.table":{"col":"hash"}}} — tables/exclude_tables are anchored POSIX regexes selecting a subset of tables; masks applies per-table in-line masking during the COPY stream (v4.4.0).';

COMMENT ON FUNCTION pgclone.database(TEXT, BOOLEAN, TEXT) IS 'Clone database with JSON options — supports the same "tables", "exclude_tables", and "masks" options as pgclone.schema (v4.4.0); masks keys may be schema-qualified ("schema.table") to disambiguate.';
