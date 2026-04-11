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

PG="psql -U postgres -d target_db -tAc"
SOURCE_CONNINFO="${SOURCE_CONNINFO:-host=source-db dbname=source_db user=postgres password=testpass}"

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
RESULT=$($PG "SELECT pgclone_clone_roles('${SOURCE_CONNINFO}');" 2>/dev/null || echo "ERROR")
run_test "pgclone_clone_roles runs without error" "[ '$RESULT' != 'ERROR' ]"

run_test "test_reader role exists" \
    "[ \"$($PG \"SELECT 1 FROM pg_roles WHERE rolname = 'test_reader';\" 2>/dev/null)\" = '1' ]"

run_test "test_writer role exists" \
    "[ \"$($PG \"SELECT 1 FROM pg_roles WHERE rolname = 'test_writer';\" 2>/dev/null)\" = '1' ]"

run_test "test_admin role exists" \
    "[ \"$($PG \"SELECT 1 FROM pg_roles WHERE rolname = 'test_admin';\" 2>/dev/null)\" = '1' ]"

run_test "test_reader has LOGIN" \
    "[ \"$($PG \"SELECT rolcanlogin FROM pg_roles WHERE rolname = 'test_reader';\" 2>/dev/null)\" = 't' ]"

run_test "test_admin has CREATEDB" \
    "[ \"$($PG \"SELECT rolcreatedb FROM pg_roles WHERE rolname = 'test_admin';\" 2>/dev/null)\" = 't' ]"

# ---- Clone verification ----
echo ""
echo "---- Clone verification ----"
RESULT=$($PG "SELECT count(*) FROM pgclone_verify('${SOURCE_CONNINFO}', 'test_schema');" 2>/dev/null || echo "0")
run_test "pgclone_verify returns rows" "[ '$RESULT' -ge 1 ]"

MATCH=$($PG "SELECT match FROM pgclone_verify('${SOURCE_CONNINFO}', 'test_schema') WHERE table_name = 'customers' LIMIT 1;" 2>/dev/null || echo "")
run_test "customers table shows match" "[ '$MATCH' = '✓' ]"

RESULT2=$($PG "SELECT count(*) FROM pgclone_verify('${SOURCE_CONNINFO}');" 2>/dev/null || echo "0")
run_test "pgclone_verify all-schemas works" "[ '$RESULT2' -ge 1 ]"

MATCH2=$($PG "SELECT match FROM pgclone_verify('${SOURCE_CONNINFO}', 'public') WHERE table_name = 'simple_test' LIMIT 1;" 2>/dev/null || echo "")
run_test "simple_test shows match" "[ '$MATCH2' = '✓' ]"

# ---- GDPR masking report ----
echo ""
echo "---- GDPR masking report ----"
RESULT=$($PG "SELECT count(*) FROM pgclone_masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'email';" 2>/dev/null || echo "0")
run_test "report detects email column" "[ '$RESULT' = '1' ]"

SENS=$($PG "SELECT sensitivity FROM pgclone_masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'full_name' LIMIT 1;" 2>/dev/null || echo "")
run_test "full_name detected as PII - Name" "[ '$SENS' = 'PII - Name' ]"

SENS2=$($PG "SELECT sensitivity FROM pgclone_masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'ssn' LIMIT 1;" 2>/dev/null || echo "")
run_test "ssn detected as National ID" "[ '$SENS2' = 'National ID' ]"

STATUS=$($PG "SELECT mask_status FROM pgclone_masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'email' LIMIT 1;" 2>/dev/null || echo "")
run_test "email shows UNMASKED" "[ '$STATUS' = 'UNMASKED' ]"

# ---- Dynamic data masking ----
echo ""
echo "---- Dynamic data masking ----"

# Clone employees for DDM test (fresh, outside transaction)
$PG "DROP TABLE IF EXISTS test_schema.employees_ddm CASCADE;" 2>/dev/null || true
RESULT=$($PG "SELECT pgclone_table('${SOURCE_CONNINFO}', 'test_schema', 'employees', true, 'employees_ddm');" 2>/dev/null || echo "ERROR")
run_test "clone employees for DDM" "[ '$RESULT' != 'ERROR' ]"

RESULT=$($PG "SELECT pgclone_create_masking_policy('test_schema', 'employees_ddm', '{\"email\": \"email\", \"full_name\": \"name\", \"ssn\": \"null\"}', 'postgres');" 2>/dev/null || echo "ERROR")
run_test "create_masking_policy runs" "[ '$RESULT' != 'ERROR' ]"

VIEW_EXISTS=$($PG "SELECT 1 FROM pg_views WHERE schemaname = 'test_schema' AND viewname = 'employees_ddm_masked';" 2>/dev/null || echo "0")
run_test "masked view exists" "[ '$VIEW_EXISTS' = '1' ]"

MASKED=$($PG "SELECT count(*) FROM test_schema.employees_ddm_masked WHERE full_name = 'XXXX';" 2>/dev/null || echo "0")
run_test "masked view shows XXXX for names" "[ '$MASKED' = '5' ]"

NULLS=$($PG "SELECT count(*) FROM test_schema.employees_ddm_masked WHERE ssn IS NULL;" 2>/dev/null || echo "0")
run_test "masked view shows NULL for SSNs" "[ '$NULLS' = '5' ]"

ROWS=$($PG "SELECT count(*) FROM test_schema.employees_ddm_masked;" 2>/dev/null || echo "0")
run_test "masked view has 5 rows" "[ '$ROWS' = '5' ]"

# Drop policy
RESULT=$($PG "SELECT pgclone_drop_masking_policy('test_schema', 'employees_ddm');" 2>/dev/null || echo "ERROR")
run_test "drop_masking_policy runs" "[ '$RESULT' != 'ERROR' ]"

# Cleanup
$PG "DROP TABLE IF EXISTS test_schema.employees_ddm CASCADE;" 2>/dev/null || true

echo ""
echo "============================================"
echo "LOOPBACK TESTS: $PASS passed, $FAIL failed"
echo "============================================"

[ $FAIL -eq 0 ] || exit 1
