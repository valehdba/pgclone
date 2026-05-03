/* pgclone--4.0.0--4.0.1.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.0.1 is a bugfix release for issue #3 (schema clone dependency ordering
-- and unqualified DDL on source DBs whose default search_path includes an
-- application schema). All changes live in the compiled .so — no SQL
-- catalog changes are required. This script is intentionally empty.
