/* pgclone--4.4.3--4.4.4.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.4.4: Auto-discover bank account numbers; remote masking_report
-- (GitHub issue #23).
--
-- Background
-- ----------
-- The issue asked for two things: (1) auto-discovery doesn't suggest
-- a mask for bank account numbers, even though they are highly
-- identifying and almost always stored as text; (2) a report of
-- sensitive columns that were auto-discovered but have no masking
-- applied, plus the reason (e.g. FK relations) — which already
-- exists as pgclone.masking_report(), but only against a local
-- (already-cloned) schema.
--
-- Fix
-- ---
--   * discover_sensitive()/masking_report() now match "bank_account",
--     "account_number", "acct_number", "iban", and common spelling
--     variants, suggesting the "null" strategy — the same
--     full-redaction treatment SSN and credit card numbers already
--     get, since a bank account number identifies a specific
--     person's account, not just a bank.
--   * masking_report() gains a 2-arg overload,
--     masking_report(source_conninfo, schema_name), so the audit can
--     run directly against a remote source (before anything is
--     cloned) the same way discover_sensitive() already does.
CREATE FUNCTION pgclone.masking_report(source_conninfo TEXT, schema_name TEXT)
RETURNS TABLE (
    schema_name    TEXT,
    table_name     TEXT,
    column_name    TEXT,
    sensitivity    TEXT,
    mask_status    TEXT,
    recommendation TEXT
) AS 'MODULE_PATHNAME', 'pgclone_masking_report'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pgclone.masking_report(TEXT, TEXT) IS 'Generate GDPR/compliance audit report directly against a remote source schema (same output as masking_report(schema_name), run before anything is cloned locally).';

-- (The bank-account pattern additions are C-only — pgclone_discover_sensitive
-- and pgclone_masking_report are existing functions, no new catalog entry
-- needed for those.)
