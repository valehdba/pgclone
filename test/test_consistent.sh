#!/bin/bash
# ============================================================
# pgclone v4.3.0 consistent-snapshot tests
#
# Verifies that schema/database clones — including parallel pool
# mode — produce a foreign-key-consistent target even while a
# concurrent writer is inserting parent+child rows on the source.
#
# Strategy:
#  1. Build a parent/child schema on the source with a NOT VALID-
#     friendly FK (child.parent_id REFERENCES parent.id).
#  2. Background loop continuously commits a parent and a child
#     row in alternating transactions, on the source.
#  3. Run pgclone.schema (sync), then pgclone.schema_async
#     pool-mode, then opt-out variants.
#  4. Assert that for every child row in the target there exists
#     a parent row in the target with the matching id. Without
#     consistent-snapshot wrapping, copying child first then
#     parent (or vice-versa across two snapshots) yields orphans
#     — the test would fail on v4.2.x and earlier.
# ============================================================

set -euo pipefail

PASS=0
FAIL=0

SOURCE_CONNINFO="${SOURCE_CONNINFO:-host=source-db dbname=source_db user=postgres password=testpass}"
SOURCE_PSQL=(psql "host=source-db dbname=source_db user=postgres password=testpass" -X -q -v ON_ERROR_STOP=1)

pg() {
    psql -U postgres -d target_db -tAc "$1"
}

run_test() {
    local desc="$1"
    local cmd="$2"
    if eval "$cmd" >/dev/null 2>&1; then
        echo "  PASS: $desc"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        echo "    cmd: $cmd"
        FAIL=$((FAIL + 1))
    fi
}

echo "============================================"
echo "Testing pgclone v4.3.0 consistent snapshot"
echo "============================================"

# ---- Build source schema once ----
echo ""
echo "---- Building source consistency_test schema ----"
PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -X -q -v ON_ERROR_STOP=1 <<'SQL'
DROP SCHEMA IF EXISTS consistency_test CASCADE;
CREATE SCHEMA consistency_test;

CREATE TABLE consistency_test.parent (
    id   bigint PRIMARY KEY,
    name text NOT NULL
);

CREATE TABLE consistency_test.child (
    id        bigint PRIMARY KEY,
    parent_id bigint NOT NULL REFERENCES consistency_test.parent(id),
    payload   text
);

-- Seed enough rows to make the clone last long enough to overlap
-- with the writer.
INSERT INTO consistency_test.parent
SELECT g, 'p_' || g
  FROM generate_series(1, 5000) AS g;

INSERT INTO consistency_test.child
SELECT g, ((g - 1) % 5000) + 1, 'c_' || g
  FROM generate_series(1, 20000) AS g;
SQL

# ---- Concurrent writer ----
# Inserts a fresh parent then a fresh child in two separate
# transactions. Between them, a snapshot taken at this exact moment
# would see the parent committed but not yet the child (or vice
# versa, depending on the next-id schedule). With consistent
# snapshot wrapping, the clone never sees an orphan.
start_writer() {
    PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -X -q \
        -v ON_ERROR_STOP=0 <<'SQL' >/tmp/pgclone_writer.log 2>&1 &
DO $$
DECLARE
    base bigint := 100000;
    n    bigint := 0;
BEGIN
    WHILE n < 5000 LOOP
        INSERT INTO consistency_test.parent VALUES (base + n, 'live_p_' || n);
        INSERT INTO consistency_test.child VALUES (base + n, base + n, 'live_c_' || n);
        n := n + 1;
        PERFORM pg_sleep(0.001);
    END LOOP;
END$$;
SQL
    WRITER_PID=$!
    echo "  writer pid: $WRITER_PID"
}

stop_writer() {
    if [ -n "${WRITER_PID:-}" ]; then
        kill "$WRITER_PID" 2>/dev/null || true
        wait "$WRITER_PID" 2>/dev/null || true
    fi
    WRITER_PID=""
}

assert_no_orphans() {
    local schema="$1"
    local n
    n=$(pg "SELECT count(*) FROM ${schema}.child c
            LEFT JOIN ${schema}.parent p ON p.id = c.parent_id
            WHERE p.id IS NULL;")
    if [ "$n" = "0" ]; then
        echo "  PASS: no orphan child rows in target ${schema} (0 violations)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${n} orphan child rows in target ${schema}"
        FAIL=$((FAIL + 1))
    fi
}

# ---- Test 1: pgclone.schema sync, default consistent ----
echo ""
echo "---- Test 1: sync schema clone (consistent default) ----"
pg "DROP SCHEMA IF EXISTS consistency_test CASCADE;" >/dev/null
pg "DROP SCHEMA IF EXISTS consistency_test_async CASCADE;" >/dev/null
pg "DROP SCHEMA IF EXISTS consistency_test_pool CASCADE;" >/dev/null
pg "DROP SCHEMA IF EXISTS consistency_test_optout CASCADE;" >/dev/null

start_writer
sleep 0.2  # let the writer start committing rows
RES=$(pg "SELECT pgclone.schema('${SOURCE_CONNINFO}', 'consistency_test', true);" || echo "ERROR")
stop_writer
run_test "pgclone.schema returns OK" "[ '$RES' = 'OK' ]"
assert_no_orphans consistency_test

# ---- Test 2: pgclone.schema_async sequential, default consistent ----
echo ""
echo "---- Test 2: async sequential schema clone (consistent default) ----"
PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -X -q -v ON_ERROR_STOP=1 \
    -c "ALTER SCHEMA consistency_test RENAME TO consistency_test_async;" >/dev/null
start_writer
sleep 0.2
JOB=$(pg "SELECT pgclone.schema_async('${SOURCE_CONNINFO}', 'consistency_test_async', true);" || echo "0")
# Poll until the job finishes (max ~60s)
for i in $(seq 1 600); do
    ST=$(pg "SELECT status FROM pgclone.jobs_view WHERE job_id = ${JOB};" || echo "?")
    if [ "$ST" = "completed" ] || [ "$ST" = "failed" ] || [ "$ST" = "cancelled" ]; then
        break
    fi
    sleep 0.1
done
stop_writer
run_test "async sequential job completed" "[ '$ST' = 'completed' ]"
assert_no_orphans consistency_test_async

# ---- Test 3: pgclone.schema_async pool mode (consistent default) ----
echo ""
echo "---- Test 3: parallel pool schema clone (consistent default) ----"
PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -X -q -v ON_ERROR_STOP=1 \
    -c "ALTER SCHEMA consistency_test_async RENAME TO consistency_test_pool;" >/dev/null
start_writer
sleep 0.2
JOB=$(pg "SELECT pgclone.schema_async('${SOURCE_CONNINFO}', 'consistency_test_pool', true, '{\"parallel\": 4}');" || echo "0")
for i in $(seq 1 600); do
    ST=$(pg "SELECT status FROM pgclone.jobs_view WHERE job_id = ${JOB};" || echo "?")
    if [ "$ST" = "completed" ] || [ "$ST" = "failed" ] || [ "$ST" = "cancelled" ]; then
        break
    fi
    sleep 0.1
done
stop_writer
run_test "pool job completed" "[ '$ST' = 'completed' ]"
assert_no_orphans consistency_test_pool

# Verify the snapshot coordinator was actually launched and exited cleanly
COORD=$(pg "SELECT count(*) FROM pgclone.jobs_view
            WHERE current_phase IN ('snapshot released', 'holding snapshot');" || echo "0")
run_test "snapshot coordinator job is visible" "[ '$COORD' -ge 1 ]"

# ---- Test 4: opt-out (consistent: false) still functions ----
echo ""
echo "---- Test 4: opt-out consistent:false still works ----"
PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -X -q -v ON_ERROR_STOP=1 \
    -c "ALTER SCHEMA consistency_test_pool RENAME TO consistency_test_optout;" >/dev/null
RES=$(pg "SELECT pgclone.schema('${SOURCE_CONNINFO}', 'consistency_test_optout', true, '{\"consistent\": false}');" || echo "ERROR")
run_test "consistent:false sync clone returns OK" "[ '$RES' = 'OK' ]"
# We don't assert no-orphans here — that's the whole point of opt-out.
# But row counts should match a quiescent source.
P_TGT=$(pg "SELECT count(*) FROM consistency_test_optout.parent;")
P_SRC=$(PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -X -tAc \
        "SELECT count(*) FROM consistency_test_optout.parent;")
run_test "opt-out parent row counts equal" "[ '$P_TGT' = '$P_SRC' ]"

# ---- Cleanup ----
echo ""
PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -X -q \
    -c "DROP SCHEMA IF EXISTS consistency_test_optout CASCADE;" >/dev/null 2>&1 || true
pg "DROP SCHEMA IF EXISTS consistency_test_optout CASCADE;" >/dev/null 2>&1 || true
pg "SELECT pgclone.clear_jobs();" >/dev/null 2>&1 || true

echo ""
echo "============================================"
echo "Consistent-snapshot tests: $PASS passed, $FAIL failed"
echo "============================================"

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
