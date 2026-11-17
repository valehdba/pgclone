#!/bin/bash
# ============================================================
# pgclone partitioned-table tests (issue #38)
#
# A partitioned parent must be recreated with PARTITION BY (not a plain
# table), its partitions with PARTITION OF, and rows must be copied once
# (from leaf partitions only; COPY of a partitioned parent is rejected).
# Covers pgclone.schema(), schema_async() and schema_async() with a
# parallel worker pool. Requires shared_preload_libraries = 'pgclone'.
# ============================================================

set -e

SOURCE_CONNINFO="host=source-db dbname=source_db user=postgres password=testpass"
PASS=0
FAIL=0

check() {
    local name="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name"
        echo "  expected: '$expected'"
        echo "  actual:   '$actual'"
        FAIL=$((FAIL + 1))
    fi
}

q()  { psql -U postgres -d target_db -tAc "$1" 2>/dev/null | tr -d '[:space:]'; }
src() { PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -q -c "$1" >/dev/null; }

echo "============================================"
echo "Testing pgclone partitioned-table cloning"
echo "============================================"

# Source fixture. a_2025 sorts before its parent "m" by name on purpose.
PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -q >/dev/null <<SQL
DROP SCHEMA IF EXISTS ps_part CASCADE;
CREATE SCHEMA ps_part;
CREATE TABLE ps_part.m (id int, d date, PRIMARY KEY (id, d)) PARTITION BY RANGE (d);
CREATE INDEX m_d_idx ON ps_part.m (d);
CREATE TABLE ps_part.m_2024 PARTITION OF ps_part.m FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE ps_part.a_2025 PARTITION OF ps_part.m FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
CREATE TABLE ps_part.m_2026 PARTITION OF ps_part.m FOR VALUES FROM ('2026-01-01') TO ('2027-01-01')
    PARTITION BY RANGE (id);
CREATE TABLE ps_part.m_2026_a PARTITION OF ps_part.m_2026 FOR VALUES FROM (0) TO (100);
INSERT INTO ps_part.m VALUES (1,'2024-02-01'),(2,'2024-03-01'),(3,'2025-02-01'),
                             (4,'2025-03-01'),(5,'2026-05-01');
SQL

reset_target() {
    psql -U postgres -d target_db -c "DROP SCHEMA IF EXISTS ps_part CASCADE" >/dev/null 2>&1
    psql -U postgres -d target_db -c "SELECT pgclone.clear_jobs()" >/dev/null 2>&1 || true
}

verify() {
    local label="$1"
    check "$label: parent is a partitioned table" "p" \
        "$(q "SELECT relkind FROM pg_class WHERE oid = 'ps_part.m'::regclass")"
    check "$label: parent holds each row exactly once (5)" "5" \
        "$(q "SELECT count(*) FROM ps_part.m")"
    check "$label: leaf partition a_2025 has 2 rows" "2" "$(q "SELECT count(*) FROM ps_part.a_2025")"
    check "$label: leaf partition m_2024 has 2 rows" "2" "$(q "SELECT count(*) FROM ps_part.m_2024")"
    check "$label: sub-partition leaf m_2026_a has 1 row" "1" "$(q "SELECT count(*) FROM ps_part.m_2026_a")"
    check "$label: sub-partitioned partition m_2026 is partitioned" "p" \
        "$(q "SELECT relkind FROM pg_class WHERE oid = 'ps_part.m_2026'::regclass")"
    check "$label: parent has 3 direct partitions" "3" \
        "$(q "SELECT count(*) FROM pg_inherits WHERE inhparent = 'ps_part.m'::regclass")"
    check "$label: primary key exists once on the parent" "1" \
        "$(q "SELECT count(*) FROM pg_constraint WHERE conrelid = 'ps_part.m'::regclass AND contype = 'p'")"
    check "$label: partitions inherit (not duplicate) the primary key" "0" \
        "$(q "SELECT count(*) FROM pg_constraint WHERE conrelid = 'ps_part.a_2025'::regclass AND contype = 'p' AND conparentid = 0")"
    check "$label: parent index exists" "1" \
        "$(q "SELECT count(*) FROM pg_indexes WHERE schemaname = 'ps_part' AND indexname = 'm_d_idx'")"
}

wait_job() {
    local job="$1" status=""
    for i in $(seq 1 60); do
        status=$(q "SELECT status FROM pgclone.jobs_view WHERE job_id = $job")
        if [ "$status" = "completed" ] || [ "$status" = "failed" ]; then break; fi
        sleep 1
    done
    echo "$status"
}

# ---- 1. synchronous schema clone ----
echo ""
echo "---- TEST 1: pgclone.schema() with a partitioned table ----"
reset_target
psql -U postgres -d target_db -q -c "SELECT pgclone.schema('${SOURCE_CONNINFO}', 'ps_part', true)" >/dev/null 2>&1 || true
verify "sync"

# ---- 2. conflict=skip rerun leaves everything alone ----
echo ""
echo "---- TEST 2: rerun with conflict=skip ----"
psql -U postgres -d target_db -q -c "SELECT pgclone.schema('${SOURCE_CONNINFO}', 'ps_part', true, '{\"conflict\": \"skip\"}')" >/dev/null 2>&1 || true
check "skip rerun: parent still holds 5 rows (no duplicates)" "5" "$(q "SELECT count(*) FROM ps_part.m")"

# ---- 3. async schema clone (sequential worker) ----
echo ""
echo "---- TEST 3: pgclone.schema_async() ----"
reset_target
JOB=$(q "SELECT pgclone.schema_async('${SOURCE_CONNINFO}', 'ps_part', true)")
check "async: job completed" "completed" "$(wait_job "$JOB")"
verify "async"

# ---- 4. async schema clone with a parallel worker pool ----
echo ""
echo "---- TEST 4: pgclone.schema_async() with parallel workers ----"
reset_target
JOB=$(q "SELECT pgclone.schema_async('${SOURCE_CONNINFO}', 'ps_part', true, '{\"parallel\": 3}')")
check "parallel async: job completed" "completed" "$(wait_job "$JOB")"
verify "parallel"

# ---- cleanup ----
src "DROP SCHEMA IF EXISTS ps_part CASCADE"
reset_target

echo ""
echo "============================================"
echo "PARTITION TEST RESULTS: $PASS passed, $FAIL failed"
echo "============================================"
if [ "$FAIL" -gt 0 ]; then
    echo "SOME PARTITION TESTS FAILED"
    exit 1
fi
echo "ALL PARTITION TESTS PASSED"
