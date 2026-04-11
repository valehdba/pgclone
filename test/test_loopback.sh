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
RESULT=$(pg "SELECT pgclone_clone_roles('${SOURCE_CONNINFO}');" || echo "ERROR")
run_test "pgclone_clone_roles runs without error" "[ '$RESULT' != 'ERROR' ]"

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
VC=$(pg "SELECT count(*) FROM pgclone_verify('${SOURCE_CONNINFO}', 'test_schema');" || echo "0")
run_test "pgclone_verify returns rows" "[ '$VC' -ge 1 ]"

MATCH=$(pg "SELECT match FROM pgclone_verify('${SOURCE_CONNINFO}', 'test_schema') WHERE table_name = 'customers' LIMIT 1;" || echo "")
run_test "customers table shows match" "[ '$MATCH' = '✓' ]"

VC2=$(pg "SELECT count(*) FROM pgclone_verify('${SOURCE_CONNINFO}');" || echo "0")
run_test "pgclone_verify all-schemas works" "[ '$VC2' -ge 1 ]"

MATCH2=$(pg "SELECT match FROM pgclone_verify('${SOURCE_CONNINFO}', 'public') WHERE table_name = 'simple_test' LIMIT 1;" || echo "")
run_test "simple_test shows match" "[ '$MATCH2' = '✓' ]"

# ---- GDPR masking report ----
echo ""
echo "---- GDPR masking report ----"
RC=$(pg "SELECT count(*) FROM pgclone_masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'email';" || echo "0")
run_test "report detects email column" "[ '$RC' = '1' ]"

SENS=$(pg "SELECT sensitivity FROM pgclone_masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'full_name' LIMIT 1;" || echo "")
run_test "full_name detected as PII - Name" "[ '$SENS' = 'PII - Name' ]"

SENS2=$(pg "SELECT sensitivity FROM pgclone_masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'ssn' LIMIT 1;" || echo "")
run_test "ssn detected as National ID" "[ '$SENS2' = 'National ID' ]"

STATUS=$(pg "SELECT mask_status FROM pgclone_masking_report('test_schema') WHERE table_name = 'employees' AND column_name = 'email' LIMIT 1;" || echo "")
run_test "email shows UNMASKED" "[ '$STATUS' = 'UNMASKED' ]"

# ---- Dynamic data masking ----
echo ""
echo "---- Dynamic data masking ----"
pg "DROP TABLE IF EXISTS test_schema.employees_ddm CASCADE;" || true
RESULT=$(pg "SELECT pgclone_table('${SOURCE_CONNINFO}', 'test_schema', 'employees', true, 'employees_ddm');" || echo "ERROR")
run_test "clone employees for DDM" "[ '$RESULT' != 'ERROR' ]"

RESULT=$(pg "SELECT pgclone_create_masking_policy('test_schema', 'employees_ddm', '{\"email\": \"email\", \"full_name\": \"name\", \"ssn\": \"null\"}', 'postgres');" || echo "ERROR")
run_test "create_masking_policy runs" "[ '$RESULT' != 'ERROR' ]"

VIEW_EXISTS=$(pg "SELECT 1 FROM pg_views WHERE schemaname = 'test_schema' AND viewname = 'employees_ddm_masked';" || echo "0")
run_test "masked view exists" "[ '$VIEW_EXISTS' = '1' ]"

MASKED=$(pg "SELECT count(*) FROM test_schema.employees_ddm_masked WHERE full_name = 'XXXX';" || echo "0")
run_test "masked view shows XXXX for names" "[ '$MASKED' = '5' ]"

NULLS=$(pg "SELECT count(*) FROM test_schema.employees_ddm_masked WHERE ssn IS NULL;" || echo "0")
run_test "masked view shows NULL for SSNs" "[ '$NULLS' = '5' ]"

ROWS=$(pg "SELECT count(*) FROM test_schema.employees_ddm_masked;" || echo "0")
run_test "masked view has 5 rows" "[ '$ROWS' = '5' ]"

RESULT=$(pg "SELECT pgclone_drop_masking_policy('test_schema', 'employees_ddm');" || echo "ERROR")
run_test "drop_masking_policy runs" "[ '$RESULT' != 'ERROR' ]"

pg "DROP TABLE IF EXISTS test_schema.employees_ddm CASCADE;" || true

echo ""
echo "============================================"
echo "LOOPBACK TESTS: $PASS passed, $FAIL failed"
echo "============================================"

[ $FAIL -eq 0 ] || exit 1
