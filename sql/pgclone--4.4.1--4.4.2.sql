/* pgclone--4.4.1--4.4.2.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.4.2: Constraint- and length-aware data masking (GitHub issue #18).
--
-- No SQL signature changes — the fix lives entirely in the C masking
-- engine (src/pgclone.c), so this upgrade script only bumps the
-- installed version.
--
-- Background
-- ----------
-- v4.4.1 (issue #17) made masking skip a strategy whose *type* a column
-- could not store. Issue #18 reported three further ways a mask could
-- still break a clone:
--   1. length  — a text/constant/partial value longer than a
--                 varchar(N)/char(N) column -> "value too long".
--   2. constant — the default 'REDACTED' (or any text) fed into a numeric
--                 column -> "invalid input syntax".
--   3. constraints — collapsing a UNIQUE/PRIMARY KEY column to one value,
--                 nulling a NOT NULL column, or masking a FOREIGN KEY
--                 column -> duplicate-key / not-null / FK violation.
--
-- Fix
-- ---
--   * Length: text (and any) mask output for a length-limited string
--     column is clamped with left(..., N) so it always fits.
--   * Constant: a "constant" mask is skipped on a non-string column
--     unless the literal is a valid number for a numeric column.
--   * NOT NULL: a "null" mask on a NOT NULL column is skipped.
--   * UNIQUE/PK: only the injective "hash" strategy is applied; every
--     value-collapsing or collision-prone strategy is skipped.
--   * FOREIGN KEY: masking a FK column is skipped (referential integrity).
--   * discover_sensitive() / masking_report() only suggest strategies the
--     engine will actually apply: FK columns are omitted, and UNIQUE/PK or
--     NOT NULL sensitive columns are steered to "hash".
--   A masked view (create_masking_policy) enforces no constraints, so only
--   the type/constant checks apply there.

-- (No catalog changes.)
