/* pgclone--4.4.0--4.4.1.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.4.1: Type-aware data masking (GitHub issue #17).
--
-- No SQL signature changes — the fix lives entirely in the C masking
-- engine (src/pgclone.c), so this upgrade script only bumps the
-- installed version.
--
-- Background
-- ----------
-- Every mask strategy emits a value of a fixed kind: a text string
-- (email/name/phone/partial/hash), an integer (random_int), SQL NULL
-- (null), or a caller-supplied literal (constant). When that value is
-- fed into a column whose type cannot parse it, the clone aborted deep
-- inside the streaming COPY, e.g. a numeric random_int mask applied to a
-- BOOLEAN column named to match the %income% financial pattern:
--
--   ERROR:  pgclone: COPY completed with error:
--           ERROR:  invalid input syntax for type boolean: "60629"
--   CONTEXT:  COPY acquired_right, line 1,
--             column base_income_replacement_allow: "60629"
--
-- Fix
-- ---
--   * Every mask-application site (schema/table/database clones,
--     mask_in_place, create_masking_policy) now checks the column's
--     pg_type.typcategory before applying a mask. An incompatible mask
--     is skipped with a WARNING and the column is emitted unchanged,
--     instead of corrupting the COPY stream. Text masks fit string
--     columns; random_int fits numeric or string columns; null and
--     constant are left to the server.
--   * pgclone.discover_sensitive() no longer suggests a mask that is
--     incompatible with a column's type (so a re-generated mask file is
--     safe to apply verbatim), and pgclone.masking_report() flags such
--     columns for manual review rather than recommending a mask that
--     would fail.

-- (No catalog changes.)
