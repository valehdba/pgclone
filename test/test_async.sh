#!/bin/bash
# ============================================================
# pgclone async functions test
# Tests: pgclone_table_async, pgclone_schema_async,
#        pgclone_progress, pgclone_cancel, pgclone_clear_jobs,
#        pgclone_jobs_view
#
# Requires shared_preload_libraries = 'pgclone'
# ============================================================

set -e

echo "============================================"
echo "Testing pgclone ASYNC functions"
echo "============================================"

SOURCE_CONNINFO="host=source-db dbname=source_db user=postgres password=testpass"
PASS=0
FAIL=0

run_test() {
    local test_name="$1"
    local result
    if result=$(eval "$2" 2>&1); then
        echo "PASS: $test_name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $test_name"
        echo "  Output: $result"
        FAIL=$((FAIL + 1))
    fi
}

# Ensure pgclone is loaded in shared_preload_libraries
# The Dockerfile/run_tests.sh should handle this, but verify
echo "Checking shared memory initialization..."
SHM_CHECK=$(psql -U postgres -d target_db -tAc "SELECT pgclone_version();" 2>&1)
echo "  pgclone version: $SHM_CHECK"

# ---- Clean slate ----
echo ""
echo "Preparing clean environment..."
psql -U postgres -d target_db -c "SELECT pgclone_clear_jobs();" 2>/dev/null || true

# Drop tables/schemas created by previous sync tests (pgTAP ROLLBACK
# doesn't undo DDL done via loopback libpq connections)
psql -U postgres -d target_db <<SQL 2>/dev/null
    DROP TABLE IF EXISTS public.simple_test CASCADE;
    DROP TABLE IF EXISTS public.simple_test_copy CASCADE;
    DROP TABLE IF EXISTS public.simple_test_empty CASCADE;
    DROP TABLE IF EXISTS public.async_renamed CASCADE;
    DROP TABLE IF EXISTS test_schema.customers_lite CASCADE;
    DROP TABLE IF EXISTS test_schema.active_only CASCADE;
SQL
# Note: test_schema is NOT dropped here — TEST 3 will clone into a
# fresh schema 'async_test_schema' to avoid conflicts with sync tests

# ============================================================
# TEST 1: pgclone_table_async — basic table clone
# ============================================================
echo ""
echo "---- TEST 1: pgclone_table_async basic ----"

JOB_ID=$(psql -U postgres -d target_db -tAc "
    SELECT pgclone_table_async(
        '${SOURCE_CONNINFO}',
        'public', 'simple_test', true
    );
")

run_test "pgclone_table_async returns job_id" "[ -n '$JOB_ID' ] && [ '$JOB_ID' -gt 0 ]"

echo "  Job ID: $JOB_ID"

# Wait for completion (max 30 seconds)
echo "  Waiting for job to complete..."
for i in $(seq 1 30); do
    STATUS=$(psql -U postgres -d target_db -tAc "
        SELECT status FROM pgclone_jobs_view WHERE job_id = $JOB_ID;
    " 2>/dev/null || echo "unknown")
    STATUS=$(echo "$STATUS" | tr -d '[:space:]')
    if [ "$STATUS" = "completed" ] || [ "$STATUS" = "failed" ]; then
        break
    fi
    sleep 1
done

run_test "Job $JOB_ID completed successfully" "[ '$STATUS' = 'completed' ]"

# Verify table was cloned
ROW_COUNT=$(psql -U postgres -d target_db -tAc "SELECT count(*)::integer FROM public.simple_test;" 2>/dev/null || echo "0")
ROW_COUNT=$(echo "$ROW_COUNT" | tr -d '[:space:]')
run_test "simple_test has 5 rows (async)" "[ '$ROW_COUNT' = '5' ]"

# ============================================================
# TEST 2: pgclone_table_async with different target name
# ============================================================
echo ""
echo "---- TEST 2: pgclone_table_async with target name ----"

JOB_ID2=$(psql -U postgres -d target_db -tAc "
    SELECT pgclone_table_async(
        '${SOURCE_CONNINFO}',
        'public', 'simple_test', true,
        'async_renamed'
    );
")

echo "  Job ID: $JOB_ID2"

for i in $(seq 1 30); do
    STATUS2=$(psql -U postgres -d target_db -tAc "
        SELECT status FROM pgclone_jobs_view WHERE job_id = $JOB_ID2;
    " 2>/dev/null || echo "unknown")
    STATUS2=$(echo "$STATUS2" | tr -d '[:space:]')
    if [ "$STATUS2" = "completed" ] || [ "$STATUS2" = "failed" ]; then
        break
    fi
    sleep 1
done

run_test "Renamed table job completed" "[ '$STATUS2' = 'completed' ]"

TABLE_EXISTS=$(psql -U postgres -d target_db -tAc "
    SELECT count(*) FROM pg_tables WHERE schemaname='public' AND tablename='async_renamed';
" 2>/dev/null || echo "0")
TABLE_EXISTS=$(echo "$TABLE_EXISTS" | tr -d '[:space:]')
run_test "async_renamed table exists" "[ '$TABLE_EXISTS' = '1' ]"

ROW_COUNT2=$(psql -U postgres -d target_db -tAc "SELECT count(*)::integer FROM public.async_renamed;" 2>/dev/null || echo "0")
ROW_COUNT2=$(echo "$ROW_COUNT2" | tr -d '[:space:]')
run_test "async_renamed has 5 rows" "[ '$ROW_COUNT2' = '5' ]"

# ============================================================
# TEST 3: pgclone_schema_async — sequential mode
# ============================================================
echo ""
echo "---- TEST 3: pgclone_schema_async (sequential) ----"

# Drop test_schema so async clone has a clean target
psql -U postgres -d target_db -c "DROP SCHEMA IF EXISTS test_schema CASCADE;" 2>/dev/null || true

JOB_ID3=$(psql -U postgres -d target_db -tAc "
    SELECT pgclone_schema_async(
        '${SOURCE_CONNINFO}',
        'test_schema', true
    );
")

echo "  Job ID: $JOB_ID3"

for i in $(seq 1 60); do
    STATUS3=$(psql -U postgres -d target_db -tAc "
        SELECT status FROM pgclone_jobs_view WHERE job_id = $JOB_ID3;
    " 2>/dev/null || echo "unknown")
    STATUS3=$(echo "$STATUS3" | tr -d '[:space:]')
    if [ "$STATUS3" = "completed" ] || [ "$STATUS3" = "failed" ]; then
        break
    fi
    sleep 1
done

run_test "Schema async job completed" "[ '$STATUS3' = 'completed' ]"

# Verify tables were cloned
CUST_COUNT=$(psql -U postgres -d target_db -tAc "SELECT count(*)::integer FROM test_schema.customers;" 2>/dev/null || echo "0")
CUST_COUNT=$(echo "$CUST_COUNT" | tr -d '[:space:]')
run_test "test_schema.customers has 10 rows (async)" "[ '$CUST_COUNT' = '10' ]"

ORD_COUNT=$(psql -U postgres -d target_db -tAc "SELECT count(*)::integer FROM test_schema.orders;" 2>/dev/null || echo "0")
ORD_COUNT=$(echo "$ORD_COUNT" | tr -d '[:space:]')
run_test "test_schema.orders has 10 rows (async)" "[ '$ORD_COUNT' = '10' ]"

# ============================================================
# TEST 4: pgclone_progress — check progress JSON
# ============================================================
echo ""
echo "---- TEST 4: pgclone_progress ----"

PROGRESS=$(psql -U postgres -d target_db -tAc "SELECT pgclone_progress($JOB_ID);" 2>/dev/null || echo "null")
run_test "pgclone_progress returns JSON" "echo '$PROGRESS' | grep -q 'job_id'"

# ============================================================
# TEST 5: pgclone_jobs — list all jobs
# ============================================================
echo ""
echo "---- TEST 5: pgclone_jobs ----"

JOBS_JSON=$(psql -U postgres -d target_db -tAc "SELECT pgclone_jobs();" 2>/dev/null || echo "[]")
run_test "pgclone_jobs returns JSON array" "echo '$JOBS_JSON' | grep -q 'job_id'"

# ============================================================
# TEST 6: pgclone_jobs_view — progress view
# ============================================================
echo ""
echo "---- TEST 6: pgclone_jobs_view ----"

VIEW_COUNT=$(psql -U postgres -d target_db -tAc "SELECT count(*) FROM pgclone_jobs_view;" 2>/dev/null || echo "0")
VIEW_COUNT=$(echo "$VIEW_COUNT" | tr -d '[:space:]')
run_test "pgclone_jobs_view has rows" "[ '$VIEW_COUNT' -ge 1 ]"

# Check progress bar column
BAR_CHECK=$(psql -U postgres -d target_db -tAc "
    SELECT progress_bar FROM pgclone_jobs_view WHERE job_id = $JOB_ID LIMIT 1;
" 2>/dev/null || echo "")
run_test "progress_bar column populated" "[ -n '$BAR_CHECK' ]"

# Check elapsed_time column
TIME_CHECK=$(psql -U postgres -d target_db -tAc "
    SELECT elapsed_time FROM pgclone_jobs_view WHERE job_id = $JOB_ID LIMIT 1;
" 2>/dev/null || echo "")
TIME_CHECK=$(echo "$TIME_CHECK" | tr -d '[:space:]')
run_test "elapsed_time column populated" "[ -n '$TIME_CHECK' ]"

# ============================================================
# TEST 7: pgclone_clear_jobs
# ============================================================
echo ""
echo "---- TEST 7: pgclone_clear_jobs ----"

CLEARED=$(psql -U postgres -d target_db -tAc "SELECT pgclone_clear_jobs();" 2>/dev/null || echo "0")
CLEARED=$(echo "$CLEARED" | tr -d '[:space:]')
run_test "pgclone_clear_jobs clears completed jobs" "[ '$CLEARED' -ge 1 ]"

# Verify jobs are cleared
REMAINING=$(psql -U postgres -d target_db -tAc "SELECT count(*) FROM pgclone_jobs_view;" 2>/dev/null || echo "999")
REMAINING=$(echo "$REMAINING" | tr -d '[:space:]')
run_test "All completed jobs cleared from view" "[ '$REMAINING' = '0' ]"

# ============================================================
# TEST 8: Worker Pool — parallel schema clone with pool
# ============================================================
echo ""
echo "---- TEST 8: Worker Pool (parallel schema clone) ----"

# Drop test_schema so pool clone has a clean target
psql -U postgres -d target_db -c "DROP SCHEMA IF EXISTS test_schema CASCADE;" 2>/dev/null || true

JOB_ID8=$(psql -U postgres -d target_db -tAc "
    SELECT pgclone_schema_async(
        '${SOURCE_CONNINFO}',
        'test_schema', true,
        '{\"parallel\": 2}'
    );
")

echo "  Pool parent Job ID: $JOB_ID8"

run_test "Pool parent job_id returned" "[ -n '$JOB_ID8' ] && [ '$JOB_ID8' -gt 0 ]"

# Wait for completion (pool workers + parent finalization)
for i in $(seq 1 60); do
    STATUS8=$(psql -U postgres -d target_db -tAc "
        SELECT status FROM pgclone_jobs_view WHERE job_id = $JOB_ID8;
    " 2>/dev/null || echo "unknown")
    STATUS8=$(echo "$STATUS8" | tr -d '[:space:]')
    if [ "$STATUS8" = "completed" ] || [ "$STATUS8" = "failed" ]; then
        break
    fi
    sleep 1
done

run_test "Pool parent job completed" "[ '$STATUS8' = 'completed' ]"

# Verify tables were cloned by pool workers
POOL_CUST=$(psql -U postgres -d target_db -tAc "SELECT count(*)::integer FROM test_schema.customers;" 2>/dev/null || echo "0")
POOL_CUST=$(echo "$POOL_CUST" | tr -d '[:space:]')
run_test "Pool: test_schema.customers has 10 rows" "[ '$POOL_CUST' = '10' ]"

POOL_ORD=$(psql -U postgres -d target_db -tAc "SELECT count(*)::integer FROM test_schema.orders;" 2>/dev/null || echo "0")
POOL_ORD=$(echo "$POOL_ORD" | tr -d '[:space:]')
run_test "Pool: test_schema.orders has 10 rows" "[ '$POOL_ORD' = '10' ]"

# Verify pool workers show as separate jobs (type = table, parallel_workers = -1 sentinel)
POOL_WORKERS=$(psql -U postgres -d target_db -tAc "
    SELECT count(*) FROM pgclone_jobs_view
    WHERE job_id > $JOB_ID8 AND operation = 'table';
" 2>/dev/null || echo "0")
POOL_WORKERS=$(echo "$POOL_WORKERS" | tr -d '[:space:]')
run_test "Pool workers visible in jobs_view" "[ '$POOL_WORKERS' -ge 1 ]"

# Clean up for final test
psql -U postgres -d target_db -c "SELECT pgclone_clear_jobs();" 2>/dev/null || true

# ============================================================
# RESULTS
# ============================================================
echo ""
echo "============================================"
echo "ASYNC TEST RESULTS: $PASS passed, $FAIL failed"
echo "============================================"

if [ $FAIL -gt 0 ]; then
    echo "SOME ASYNC TESTS FAILED"
    exit 1
else
    echo "ALL ASYNC TESTS PASSED"
    exit 0
fi
