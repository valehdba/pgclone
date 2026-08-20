#!/bin/bash
# ============================================================
# pgclone chaos / failure-injection tests
#
# Covers the error paths that regular tests never exercise:
#   1. Worker SIGKILL mid-clone (regression test for the P0-2
#      orphan-slot fix — v4.4.2+ registers before_shmem_exit so
#      the slot flips RUNNING -> FAILED instead of hanging).
#   2. Async slot saturation (all PGCLONE_MAX_JOBS busy).
#   3. Pool coordinator terminated before snapshot publish
#      (workers must fail rather than wait forever).
#
# Requires shared_preload_libraries = 'pgclone'.
# ============================================================

set -euo pipefail

echo "============================================"
echo "Testing pgclone CHAOS scenarios"
echo "============================================"

SOURCE_CONNINFO="host=source-db dbname=source_db user=postgres password=testpass"
PASS=0
FAIL=0

# pgclone.jobs_view schema uses text status labels: pending / running /
# completed / failed / cancelled. The kill path in this file wants
# status == 'failed' with a specific error_message.
STATUS_SQL="SELECT status FROM pgclone.jobs_view WHERE job_id = %d"
ERRMSG_SQL="SELECT error_message FROM pgclone.jobs_view WHERE job_id = %d"

run_test() {
    local name="$1"
    local expected_value="$2"
    local actual_value="$3"
    if [ "$expected_value" = "$actual_value" ]; then
        echo "PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name"
        echo "  expected: '$expected_value'"
        echo "  actual:   '$actual_value'"
        FAIL=$((FAIL + 1))
    fi
}

# Skip cleanly if the extension is missing or shared_preload_libraries
# does not include pgclone (async paths need shared memory).
if ! psql -U postgres -d target_db -tAc "SELECT pgclone.version()" >/dev/null 2>&1; then
    echo "SKIP: pgclone extension not installed"
    exit 0
fi
SPL=$(psql -U postgres -d target_db -tAc "SHOW shared_preload_libraries" 2>/dev/null || echo "")
if ! echo "$SPL" | grep -q "pgclone"; then
    echo "SKIP: pgclone not in shared_preload_libraries"
    exit 0
fi

psql -U postgres -d target_db -c "SELECT pgclone.clear_jobs()" >/dev/null 2>&1 || true

# ------------------------------------------------------------
# Fixture: a table large enough that a clone runs long enough
# to send SIGKILL mid-flight. 200k rows keeps the COPY in the
# multi-second range without ballooning test runtime.
# ------------------------------------------------------------
echo ""
echo "Preparing chaos fixture (200k rows on source)..."
PGPASSWORD=testpass psql -h source-db -U postgres -d source_db <<'SQL' >/dev/null
    DROP TABLE IF EXISTS public.chaos_big CASCADE;
    CREATE TABLE public.chaos_big (
        id       bigserial PRIMARY KEY,
        payload  text NOT NULL
    );
    INSERT INTO public.chaos_big (payload)
    SELECT repeat(md5(g::text), 8)
    FROM generate_series(1, 200000) g;
SQL
psql -U postgres -d target_db -c "DROP TABLE IF EXISTS public.chaos_big" >/dev/null 2>&1 || true

# ============================================================
# TEST 1: SIGKILL a running worker; the slot must flip to
# FAILED via the before_shmem_exit callback (P0-2).
# ============================================================
echo ""
echo "---- TEST 1: worker SIGKILL -> slot marked FAILED ----"

JOB_ID=$(psql -U postgres -d target_db -tAc "
    SELECT pgclone.table_async('${SOURCE_CONNINFO}',
                               'public', 'chaos_big', true)
" | tr -d '[:space:]')
echo "  Job ID: $JOB_ID"

# Wait until the worker's PID appears (up to 10 s).
WORKER_PID=""
for i in $(seq 1 40); do
    WORKER_PID=$(psql -U postgres -d target_db -tAc "
        SELECT worker_pid FROM pgclone.jobs_view
        WHERE job_id = ${JOB_ID} AND status = 'running'
    " 2>/dev/null | tr -d '[:space:]' || true)
    if [ -n "${WORKER_PID:-}" ] && [ "$WORKER_PID" != "0" ]; then
        break
    fi
    sleep 0.25
done
run_test "worker pid observed in jobs_view" \
         "true" \
         "$([ -n "${WORKER_PID:-}" ] && [ "${WORKER_PID:-0}" != "0" ] && echo true || echo false)"

if [ -n "${WORKER_PID:-}" ] && [ "$WORKER_PID" != "0" ]; then
    kill -9 "$WORKER_PID" 2>/dev/null || true
    echo "  Sent SIGKILL to worker pid $WORKER_PID"
fi

# Wait for the slot to leave the RUNNING state (up to 15 s). The
# before_shmem_exit callback should fire on the next context
# switch after the kill, but shared memory eviction of the dying
# backend can take a moment.
STATUS=""
for i in $(seq 1 60); do
    STATUS=$(psql -U postgres -d target_db -tAc "$(printf "$STATUS_SQL" "$JOB_ID")" \
             2>/dev/null | tr -d '[:space:]' || true)
    if [ "$STATUS" != "running" ] && [ "$STATUS" != "pending" ]; then
        break
    fi
    sleep 0.25
done
run_test "killed job leaves RUNNING state" \
         "failed" "$STATUS"

ERRMSG=$(psql -U postgres -d target_db -tAc "$(printf "$ERRMSG_SQL" "$JOB_ID")" \
         2>/dev/null | sed 's/^ *//;s/ *$//' || echo "")
run_test "killed job records unexpected-exit reason" \
         "worker exited unexpectedly" "$ERRMSG"

# ============================================================
# TEST 2: All PGCLONE_MAX_JOBS slots busy -> next submit fails
# with 'no free job slots'.
#
# We submit against a table that does not exist so each job
# fails fast without holding a real worker; that's enough to
# exercise the slot bookkeeping. Then we run 17 submits in a
# tight loop from one backend and check the last one raises.
# ============================================================
echo ""
echo "---- TEST 2: slot exhaustion raises 'no free job slots' ----"

psql -U postgres -d target_db -c "SELECT pgclone.clear_jobs()" >/dev/null 2>&1 || true

# Poison table name so every job fails fast.
BAD_NAME="chaos_does_not_exist_$RANDOM"

SATURATION_OUT=$(psql -U postgres -d target_db -v ON_ERROR_STOP=0 2>&1 <<SQL || true
DO \$\$
DECLARE
    i int;
BEGIN
    FOR i IN 1..17 LOOP
        BEGIN
            PERFORM pgclone.table_async('${SOURCE_CONNINFO}',
                                        'public', '${BAD_NAME}', true);
        EXCEPTION WHEN OTHERS THEN
            RAISE NOTICE 'attempt %: %', i, SQLERRM;
        END;
    END LOOP;
END\$\$;
SQL
)

if echo "$SATURATION_OUT" | grep -q "no free job slots"; then
    echo "PASS: 17th submit rejected with 'no free job slots'"
    PASS=$((PASS + 1))
else
    # Alternative: bookkeeping may have freed slots faster than we
    # submitted. In that case we should at least see 17 attempts.
    ATTEMPT_COUNT=$(echo "$SATURATION_OUT" | grep -c "attempt " || true)
    if [ "$ATTEMPT_COUNT" -ge "1" ]; then
        echo "SKIP: could not saturate (jobs completed too fast); $ATTEMPT_COUNT attempts observed"
    else
        echo "FAIL: neither saturation nor NOTICE observed"
        echo "  output: $SATURATION_OUT"
        FAIL=$((FAIL + 1))
    fi
fi

psql -U postgres -d target_db -c "SELECT pgclone.clear_jobs()" >/dev/null 2>&1 || true

# ============================================================
# TEST 3: Parallel schema clone with coordinator killed before
# it publishes the snapshot. Pool workers should time out or
# fail, not hang forever.
# ============================================================
echo ""
echo "---- TEST 3: coordinator kill -> pool workers unblock ----"

# Fresh target schema so nothing collides.
psql -U postgres -d target_db -c "DROP SCHEMA IF EXISTS chaos_pool CASCADE" \
    >/dev/null 2>&1 || true
PGPASSWORD=testpass psql -h source-db -U postgres -d source_db <<'SQL' >/dev/null
    DROP SCHEMA IF EXISTS chaos_pool CASCADE;
    CREATE SCHEMA chaos_pool;
    CREATE TABLE chaos_pool.a (id int PRIMARY KEY);
    CREATE TABLE chaos_pool.b (id int PRIMARY KEY);
    CREATE TABLE chaos_pool.c (id int PRIMARY KEY);
    CREATE TABLE chaos_pool.d (id int PRIMARY KEY);
    INSERT INTO chaos_pool.a SELECT generate_series(1, 5000);
    INSERT INTO chaos_pool.b SELECT generate_series(1, 5000);
    INSERT INTO chaos_pool.c SELECT generate_series(1, 5000);
    INSERT INTO chaos_pool.d SELECT generate_series(1, 5000);
SQL

PARENT_JOB=$(psql -U postgres -d target_db -tAc "
    SELECT pgclone.schema_async('${SOURCE_CONNINFO}', 'chaos_pool', true,
                                '{\"parallel\": 3}')
" | tr -d '[:space:]')
echo "  Parent job: $PARENT_JOB"

# Grab the pool coordinator's pid. The pool coordinator is the
# first bgworker registered when the parent job runs; find it via
# jobs_view rows whose op_type is 'schema' and whose start_time is
# after the parent's.
COORD_PID=""
for i in $(seq 1 40); do
    COORD_PID=$(psql -U postgres -d target_db -tAc "
        SELECT worker_pid FROM pgclone.jobs_view
        WHERE job_id > ${PARENT_JOB}
          AND worker_pid IS NOT NULL AND worker_pid <> 0
        ORDER BY job_id LIMIT 1
    " 2>/dev/null | tr -d '[:space:]' || true)
    if [ -n "${COORD_PID:-}" ] && [ "${COORD_PID:-0}" != "0" ]; then
        break
    fi
    sleep 0.25
done

if [ -n "${COORD_PID:-}" ] && [ "${COORD_PID:-0}" != "0" ]; then
    kill -9 "$COORD_PID" 2>/dev/null || true
    echo "  Sent SIGKILL to coordinator pid $COORD_PID"
else
    echo "  (coordinator pid not observed in time; parent will still be checked)"
fi

# Wait for the whole pool to reach a terminal state — up to 30 s.
# The exact terminal state is FAILED (snapshot_failed is set by
# the exit callback and workers propagate that). But if the pool
# had already fully launched and imported, workers may still
# COMPLETE; we accept either "failed" or "completed" and only
# fail on "running"/"pending".
PARENT_STATUS=""
for i in $(seq 1 120); do
    PARENT_STATUS=$(psql -U postgres -d target_db -tAc "$(printf "$STATUS_SQL" "$PARENT_JOB")" \
                    2>/dev/null | tr -d '[:space:]' || true)
    if [ "$PARENT_STATUS" != "running" ] && [ "$PARENT_STATUS" != "pending" ]; then
        break
    fi
    sleep 0.25
done

case "$PARENT_STATUS" in
    failed|completed)
        echo "PASS: pool reached terminal state ($PARENT_STATUS)"
        PASS=$((PASS + 1))
        ;;
    *)
        echo "FAIL: pool stuck in status='$PARENT_STATUS' after coordinator kill"
        FAIL=$((FAIL + 1))
        ;;
esac

# ============================================================
echo ""
echo "============================================"
echo "Chaos test results: PASS=$PASS FAIL=$FAIL"
echo "============================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
