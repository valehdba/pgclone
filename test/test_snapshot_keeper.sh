#!/bin/bash
# ============================================================
# pgclone v4.3.1 snapshot-keeper resilience test (issue #9)
#
# Reproduces the failure mode where the snapshot keeper transaction
# is killed by a non-zero idle_in_transaction_session_timeout on
# the source, leading to "ERROR: pgclone: could not import
# snapshot ... invalid snapshot identifier" on the next per-table
# importer.
#
# Strategy:
#  1. Build a small multi-table schema on the source.
#  2. Set idle_in_transaction_session_timeout = 1s on the role
#     used by the test.
#  3. Run pgclone.schema(). With the v4.3.1 fix the keeper's
#     BEGIN issues SET LOCAL idle_in_transaction_session_timeout=0
#     and the clone completes. Without the fix, the keeper is
#     killed during the per-table phase and a later importer
#     fails with "invalid snapshot identifier".
# ============================================================

set -euo pipefail

PASS=0
FAIL=0

SOURCE_HOST="${SOURCE_HOST:-source-db}"
SOURCE_PORT="${SOURCE_PORT:-5432}"
SOURCE_DB="${SOURCE_DB:-source_db}"
SOURCE_USER="${SOURCE_USER:-postgres}"
SOURCE_PW="${SOURCE_PW:-testpass}"

src() {
    PGPASSWORD="$SOURCE_PW" psql -h "$SOURCE_HOST" -p "$SOURCE_PORT" \
        -U "$SOURCE_USER" -d "$SOURCE_DB" \
        -X -q -v ON_ERROR_STOP=1 "$@"
}

tgt() {
    psql -U postgres -d target_db -tAc "$1"
}

run_test() {
    local desc="$1"
    local cmd="$2"
    if eval "$cmd"; then
        echo "  PASS: $desc"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        echo "    cmd: $cmd"
        FAIL=$((FAIL + 1))
    fi
}

echo "============================================"
echo "Testing pgclone v4.3.1 snapshot keeper (#9)"
echo "============================================"

# ---- Build a small multi-table schema on the source ----
echo ""
echo "---- Building source keeper_test schema ----"
src <<'SQL'
DROP SCHEMA IF EXISTS keeper_test CASCADE;
CREATE SCHEMA keeper_test;

CREATE TABLE keeper_test.t1 (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_test.t2 (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_test.t3 (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_test.t4 (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_test.t5 (id int PRIMARY KEY, payload text);

-- Enough rows per table that the per-table COPY plus loopback
-- DDL adds up to several seconds — easily exceeding the
-- 1s idle_in_transaction_session_timeout we set below.
INSERT INTO keeper_test.t1 SELECT g, repeat('x', 200) FROM generate_series(1, 5000) g;
INSERT INTO keeper_test.t2 SELECT g, repeat('y', 200) FROM generate_series(1, 5000) g;
INSERT INTO keeper_test.t3 SELECT g, repeat('z', 200) FROM generate_series(1, 5000) g;
INSERT INTO keeper_test.t4 SELECT g, repeat('w', 200) FROM generate_series(1, 5000) g;
INSERT INTO keeper_test.t5 SELECT g, repeat('v', 200) FROM generate_series(1, 5000) g;
SQL

# ---- Set a tight idle_in_transaction_session_timeout on the
# ---- source role used by pgclone. Without the v4.3.1 fix the
# ---- keeper transaction will be terminated during the
# ---- per-table loop. ALTER ROLE persists; we revert in cleanup.
echo ""
echo "---- Setting idle_in_transaction_session_timeout = 1s on source role ----"
src <<SQL
ALTER ROLE $SOURCE_USER SET idle_in_transaction_session_timeout = '1s';
SQL

cleanup() {
    src <<SQL >/dev/null 2>&1 || true
ALTER ROLE $SOURCE_USER RESET idle_in_transaction_session_timeout;
DROP SCHEMA IF EXISTS keeper_test CASCADE;
SQL
    tgt "DROP SCHEMA IF EXISTS keeper_test CASCADE" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# ---- Clean target schema if a previous run left it ----
tgt "DROP SCHEMA IF EXISTS keeper_test CASCADE" >/dev/null

# ---- Run the schema clone ----
echo ""
echo "---- Running pgclone.schema(...) ----"
CONNINFO="host=$SOURCE_HOST port=$SOURCE_PORT dbname=$SOURCE_DB user=$SOURCE_USER password=$SOURCE_PW"

CLONE_RC=0
CLONE_OUT=$(tgt "SELECT pgclone.schema('$CONNINFO', 'keeper_test', true)" 2>&1) || CLONE_RC=$?

echo "  exit code: $CLONE_RC"

run_test "pgclone.schema returns OK under tight idle_in_transaction_session_timeout" \
    "[ '$CLONE_RC' = '0' ] && [ '$CLONE_OUT' = 'OK' ]"

run_test "no 'invalid snapshot identifier' error in clone output" \
    "! echo '$CLONE_OUT' | grep -qi 'invalid snapshot identifier'"

# ---- Verify all 5 tables made it and row counts match ----
for tbl in t1 t2 t3 t4 t5; do
    n=$(tgt "SELECT count(*) FROM keeper_test.$tbl" 2>/dev/null || echo "missing")
    run_test "keeper_test.$tbl was cloned with 5000 rows (actual: $n)" \
        "[ '$n' = '5000' ]"
done

echo ""
echo "============================================"
echo "Results: $PASS passed, $FAIL failed"
echo "============================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
