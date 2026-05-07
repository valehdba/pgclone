#!/bin/bash
# ============================================================
# pgclone loopback-DDL tests
#
# Tests that use loopback connections for DDL (CREATE VIEW,
# CREATE ROLE, GRANT, etc.) cannot run inside pgTAP's transaction
# because the loopback session may deadlock with locks held by
# the test transaction.
# ============================================================

set -e

PASS=0
FAIL=0

SOURCE_CONNINFO="${SOURCE_CONNINFO:-host=source-db dbname=source_db user=postgres password=testpass}"

pg() {
    psql -U postgres -d target_db -tAc "$1" 2>/dev/null
}

run_test() {
    local desc="$1"
    local cmd="$2"
    if eval "$cmd" 2>/dev/null; then
        echo "  PASS: $desc"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"; FAIL=$((FAIL + 1))
    fi
}

echo "============================================"
echo "Testing pgclone loopback-DDL functions"
echo "============================================"

# ---- Clone roles ----
echo ""
echo "---- Clone roles ----"
RESULT=$(pg "SELECT pgclone.clone_roles('${SOURCE_CONNINFO}');" || echo "ERROR")
run_test "pgclone.clone_roles runs without error" "[ '$RESULT' != 'ERROR' ]"

R1=$(pg "SELECT 1 FROM pg_roles WHERE rolname = 'test_reader';" || echo "0")
run_test "test_reader role exists" "[ '$R1' = '1' ]"

R2=$(pg "SELECT 1 FROM pg_roles WHERE rolname = 'test_writer';" || echo "0")
run_test "test_writer role exists" "[ '$R2' = '1' ]"

R3=$(pg "SELECT 1 FROM pg_roles WHERE rolname = 'test_admin';" || echo "0")
run_test "test_admin role exists" "[ '$R3' = '1' ]"

R4=$(pg "SELECT rolcanlogin FROM pg_roles WHERE rolname = 'test_reader';" || echo "f")
run_test "test_reader has LOGIN" "[ '$R4' = 't' ]"

R5=$(pg "SELECT rolcreatedb FROM pg_roles WHERE rolname = 'test_admin';" || echo "f")
run_test "test_admin has CREATEDB" "[ '$R5' = 't' ]"

# ---- Clone verification ----
echo ""
echo "---- Clone verification ----"
VC=$(pg "SELECT count(*) FROM pgclone.verify('${SOURCE_CONNINFO}', 'test_schema');" || echo "0")
run_test "pgclone.verify returns rows" "[ '$VC' -ge 1 ]"

MATCH=$(pg "SELECT match FROM pgclone.verify('${SOURCE_CONNINFO}', 'test_schema') WHERE table_name = 'customers' LIMIT 1;" || echo "MISSING")
run_test "customers table shows match" "echo '$MATCH' | grep -qv 'missing'"

VC2=$(pg "SELECT count(*) FROM pgclone.verify('${SOURCE_CONNINFO}');" || echo "0")
run_test "pgclone.verify all-schemas works" "[ '$VC2' -ge 1 ]"

MATCH2=$(pg "SELECT match FROM pgclone.verify('${SOURCE_CONNINFO}', 'public') WHERE table_name = 'simple_test' LIMIT 1;" || echo "MISSING")
run_test "simple_test shows match" "echo '$MATCH2' | grep -qv 'missing'"

# ---- GDPR masking report ----
echo ""
echo "---- GDPR masking report ----"
RC=$(pg "SELECT count(*) FROM pgclone.masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'email';" || echo "0")
run_test "report detects email column" "[ '$RC' = '1' ]"

SENS=$(pg "SELECT sensitivity FROM pgclone.masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'full_name' LIMIT 1;" || echo "")
run_test "full_name detected as PII - Name" "[ '$SENS' = 'PII - Name' ]"

SENS2=$(pg "SELECT sensitivity FROM pgclone.masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'ssn' LIMIT 1;" || echo "")
run_test "ssn detected as National ID" "[ '$SENS2' = 'National ID' ]"

STATUS=$(pg "SELECT mask_status FROM pgclone.masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'email' LIMIT 1;" || echo "")
run_test "email shows UNMASKED" "[ '$STATUS' = 'UNMASKED' ]"

# ---- Dynamic data masking ----
echo ""
echo "---- Dynamic data masking ----"
pg "DROP TABLE IF EXISTS test_schema.employees_ddm CASCADE;" || true
RESULT=$(pg "SELECT pgclone.table('${SOURCE_CONNINFO}', 'test_schema', 'employees', true, 'employees_ddm');" || echo "ERROR")
run_test "clone employees for DDM" "[ '$RESULT' != 'ERROR' ]"

RESULT=$(pg "SELECT pgclone.create_masking_policy('test_schema', 'employees_ddm', '{\"email\": \"email\", \"full_name\": \"name\", \"ssn\": \"null\"}', 'postgres');" || echo "ERROR")
run_test "create_masking_policy runs" "[ '$RESULT' != 'ERROR' ]"

VIEW_EXISTS=$(pg "SELECT 1 FROM pg_views WHERE schemaname = 'test_schema' AND viewname = 'employees_ddm_masked';" || echo "0")
run_test "masked view exists" "[ '$VIEW_EXISTS' = '1' ]"

MASKED=$(pg "SELECT count(*) FROM test_schema.employees_ddm_masked WHERE full_name = 'XXXX';" || echo "0")
run_test "masked view shows XXXX for names" "[ '$MASKED' = '5' ]"

NULLS=$(pg "SELECT count(*) FROM test_schema.employees_ddm_masked WHERE ssn IS NULL;" || echo "0")
run_test "masked view shows NULL for SSNs" "[ '$NULLS' = '5' ]"

ROWS=$(pg "SELECT count(*) FROM test_schema.employees_ddm_masked;" || echo "0")
run_test "masked view has 5 rows" "[ '$ROWS' = '5' ]"

RESULT=$(pg "SELECT pgclone.drop_masking_policy('test_schema', 'employees_ddm');" || echo "ERROR")
run_test "drop_masking_policy runs" "[ '$RESULT' != 'ERROR' ]"

pg "DROP TABLE IF EXISTS test_schema.employees_ddm CASCADE;" || true

# ---- Schema diff (v4.1.0) ----
echo ""
echo "---- Schema diff ----"

# Function exists under pgclone schema
DIFF_EXISTS=$(pg "SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = 'pgclone' AND p.proname = 'diff' AND pg_catalog.pg_get_function_arguments(p.oid) = 'source_conninfo text, schema_name text';" || echo "0")
run_test "pgclone.diff(text, text) is registered" "[ '$DIFF_EXISTS' = '1' ]"

# Returns parseable JSON with the documented top-level keys
HAS_TABLES=$(pg "SELECT pgclone.diff('${SOURCE_CONNINFO}', 'test_schema')::jsonb ? 'tables';" || echo "f")
run_test "diff JSON contains 'tables' key" "[ '$HAS_TABLES' = 't' ]"

HAS_INDEXES=$(pg "SELECT pgclone.diff('${SOURCE_CONNINFO}', 'test_schema')::jsonb ? 'indexes';" || echo "f")
run_test "diff JSON contains 'indexes' key" "[ '$HAS_INDEXES' = 't' ]"

HAS_SUMMARY=$(pg "SELECT pgclone.diff('${SOURCE_CONNINFO}', 'test_schema')::jsonb ? 'summary';" || echo "f")
run_test "diff JSON contains 'summary' key" "[ '$HAS_SUMMARY' = 't' ]"

# Schema name echoed back
SCHEMA_FIELD=$(pg "SELECT pgclone.diff('${SOURCE_CONNINFO}', 'test_schema')::jsonb ->> 'schema';" || echo "")
run_test "diff echoes the requested schema" "[ '$SCHEMA_FIELD' = 'test_schema' ]"

# in_sync is a boolean
IN_SYNC=$(pg "SELECT jsonb_typeof(pgclone.diff('${SOURCE_CONNINFO}', 'test_schema')::jsonb -> 'in_sync');" || echo "")
run_test "diff in_sync is a boolean" "[ '$IN_SYNC' = 'boolean' ]"

# Fabricate a target-only table; diff must surface it under tables.only_in_target
pg "DROP TABLE IF EXISTS test_schema.pgclone_diff_probe CASCADE;" || true
pg "CREATE TABLE test_schema.pgclone_diff_probe (id int);" || true
ONLY_TGT=$(pg "SELECT (pgclone.diff('${SOURCE_CONNINFO}', 'test_schema')::jsonb #> '{tables,only_in_target}') ? 'pgclone_diff_probe';" || echo "f")
run_test "diff detects fabricated target-only table" "[ '$ONLY_TGT' = 't' ]"

# diff_count must reflect the fabricated drift (>= 1)
DC=$(pg "SELECT (pgclone.diff('${SOURCE_CONNINFO}', 'test_schema')::jsonb ->> 'diff_count')::int;" || echo "0")
run_test "diff_count is positive when drift exists" "[ '$DC' -ge 1 ]"

pg "DROP TABLE IF EXISTS test_schema.pgclone_diff_probe CASCADE;" || true

# Read-only invariant: diff must not change relation count on either side
COUNT_BEFORE=$(pg "SELECT count(*)::int FROM information_schema.tables WHERE table_schema = 'test_schema';" || echo "0")
pg "SELECT pgclone.diff('${SOURCE_CONNINFO}', 'test_schema');" >/dev/null || true
COUNT_AFTER=$(pg "SELECT count(*)::int FROM information_schema.tables WHERE table_schema = 'test_schema';" || echo "0")
run_test "diff does not modify target catalog" "[ '$COUNT_BEFORE' = '$COUNT_AFTER' ]"

# NULL schema_name argument must error (function is STRICT — psql returns no row)
NULL_RES=$(pg "SELECT pgclone.diff('${SOURCE_CONNINFO}', NULL);" || echo "ERROR")
run_test "diff is STRICT (NULL arg yields no result)" "[ -z '$NULL_RES' -o '$NULL_RES' = 'ERROR' ]"

# ---- Pre-flight validator (v4.2.0) ----
echo ""
echo "---- Pre-flight validator ----"

# Function exists with the documented (text, text) signature
PF_EXISTS=$(pg "SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = 'pgclone' AND p.proname = 'preflight' AND pg_catalog.pg_get_function_arguments(p.oid) = 'source_conninfo text, schema_name text';" || echo "0")
run_test "pgclone.preflight(text, text) is registered" "[ '$PF_EXISTS' = '1' ]"

# Top-level keys present
HAS_SCHEMA=$(pg "SELECT pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema')::jsonb ? 'schema';" || echo "f")
run_test "preflight JSON contains 'schema' key" "[ '$HAS_SCHEMA' = 't' ]"

HAS_READY=$(pg "SELECT pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema')::jsonb ? 'ready';" || echo "f")
run_test "preflight JSON contains 'ready' key" "[ '$HAS_READY' = 't' ]"

HAS_SUMMARY=$(pg "SELECT pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema')::jsonb ? 'summary';" || echo "f")
run_test "preflight JSON contains 'summary' key" "[ '$HAS_SUMMARY' = 't' ]"

HAS_CHECKS=$(pg "SELECT pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema')::jsonb ? 'checks';" || echo "f")
run_test "preflight JSON contains 'checks' key" "[ '$HAS_CHECKS' = 't' ]"

# ready must be a boolean
READY_TYPE=$(pg "SELECT jsonb_typeof(pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema')::jsonb -> 'ready');" || echo "")
run_test "preflight 'ready' is boolean" "[ '$READY_TYPE' = 'boolean' ]"

# summary contains the four documented integer counters
SUMMARY_KEYS=$(pg "SELECT (pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema')::jsonb -> 'summary') ?& ARRAY['errors','warnings','info','checks_run'];" || echo "f")
run_test "preflight summary has errors/warnings/info/checks_run" "[ '$SUMMARY_KEYS' = 't' ]"

# checks must include the connection + version + schema-existence checks
CONN_CHECK=$(pg "SELECT (pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema')::jsonb -> 'checks') ?& ARRAY['source_connection','target_connection','source_version','target_version','schema_exists_source','schema_exists_target'];" || echo "f")
run_test "preflight checks include conn/version/schema_exists entries" "[ '$CONN_CHECK' = 't' ]"

# Schema name echoed back
SCHEMA_FIELD=$(pg "SELECT pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema')::jsonb ->> 'schema';" || echo "")
run_test "preflight echoes the requested schema" "[ '$SCHEMA_FIELD' = 'test_schema' ]"

# A non-existent source schema must report not-ready with an error message
NOTREADY=$(pg "SELECT (pgclone.preflight('${SOURCE_CONNINFO}', 'no_such_schema_pgclone_pf')::jsonb ->> 'ready')::boolean;" || echo "t")
run_test "preflight reports ready=false for missing source schema" "[ '$NOTREADY' = 'f' ]"

ERR_COUNT=$(pg "SELECT jsonb_array_length((pgclone.preflight('${SOURCE_CONNINFO}', 'no_such_schema_pgclone_pf')::jsonb -> 'errors'));" || echo "0")
run_test "preflight errors[] non-empty for missing source schema" "[ '$ERR_COUNT' -ge 1 ]"

# name_conflicts.items must always be a JSON array (empty or populated)
ITEMS_TYPE=$(pg "SELECT jsonb_typeof(pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema')::jsonb #> '{checks,name_conflicts,items}');" || echo "")
run_test "preflight name_conflicts.items is an array" "[ '$ITEMS_TYPE' = 'array' ]"

# object_counts must report non-negative integers for tables / views / sequences / indexes
OC_TABLES=$(pg "SELECT (pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema')::jsonb #>> '{checks,object_counts,tables}')::int;" || echo "-1")
run_test "preflight object_counts.tables is a non-negative integer" "[ '$OC_TABLES' -ge 0 ]"

# version_compat status must be one of pass / warn (never error/skip in this env)
VC_STATUS=$(pg "SELECT pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema')::jsonb #>> '{checks,version_compat,status}';" || echo "")
run_test "preflight version_compat status is pass or warn" "[ '$VC_STATUS' = 'pass' -o '$VC_STATUS' = 'warn' ]"

# Read-only invariant on the target catalog
COUNT_BEFORE=$(pg "SELECT count(*)::int FROM information_schema.tables WHERE table_schema = 'test_schema';" || echo "0")
pg "SELECT pgclone.preflight('${SOURCE_CONNINFO}', 'test_schema');" >/dev/null || true
COUNT_AFTER=$(pg "SELECT count(*)::int FROM information_schema.tables WHERE table_schema = 'test_schema';" || echo "0")
run_test "preflight does not modify target catalog" "[ '$COUNT_BEFORE' = '$COUNT_AFTER' ]"

# STRICT: NULL schema_name argument yields no row (psql returns empty string)
NULL_RES=$(pg "SELECT pgclone.preflight('${SOURCE_CONNINFO}', NULL);" || echo "ERROR")
run_test "preflight is STRICT (NULL arg yields no result)" "[ -z '$NULL_RES' -o '$NULL_RES' = 'ERROR' ]"

echo ""
echo "============================================"
echo "LOOPBACK TESTS: $PASS passed, $FAIL failed"
echo "============================================"

[ $FAIL -eq 0 ] || exit 1
