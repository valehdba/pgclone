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

echo ""
echo "============================================"
echo "LOOPBACK TESTS: $PASS passed, $FAIL failed"
echo "============================================"

[ $FAIL -eq 0 ] || exit 1
