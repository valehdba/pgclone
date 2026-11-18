#!/bin/bash
# ============================================================
# pgclone "on_error": "continue" tests (discussion #41)
#
# A table whose target definition is incompatible with the source makes
# COPY fail. Default behavior aborts the whole schema clone; with
# on_error=continue the failing table is reported and the remaining
# tables (and their foreign keys) are still cloned.
# Requires shared_preload_libraries = 'pgclone'.
# ============================================================

set -euo pipefail

SOURCE_CONNINFO="host=source-db dbname=source_db user=postgres password=testpass"
PASS=0
FAIL=0

check() {
    local name="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "PASS: $name"; PASS=$((PASS + 1))
    else
        echo "FAIL: $name"; echo "  expected: '$expected'"; echo "  actual:   '$actual'"
        FAIL=$((FAIL + 1))
    fi
}

q()   { psql -U postgres -d target_db -tAc "$1" 2>&1 | tail -1 | tr -d '\r'; }
src() { PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -q -c "$1" >/dev/null; }

PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -q >/dev/null <<SQL
DROP SCHEMA IF EXISTS ps_coe CASCADE;
CREATE SCHEMA ps_coe;
CREATE TABLE ps_coe.parent (id int PRIMARY KEY);
CREATE TABLE ps_coe.bad  (id int PRIMARY KEY, extra text, pid int REFERENCES ps_coe.parent(id));
CREATE TABLE ps_coe.good (id int PRIMARY KEY, pid int REFERENCES ps_coe.parent(id));
INSERT INTO ps_coe.parent VALUES (1),(2);
INSERT INTO ps_coe.bad VALUES (1,'x',1);
INSERT INTO ps_coe.good VALUES (1,1),(2,2);
SQL

# Target has a narrower "bad" table, so COPY into it fails.
reset_target() {
    psql -U postgres -d target_db -q -c "DROP SCHEMA IF EXISTS ps_coe CASCADE; CREATE SCHEMA ps_coe; CREATE TABLE ps_coe.bad (id int)" >/dev/null 2>&1
}

echo "---- TEST 1: default aborts ----"
reset_target
OUT=$(psql -U postgres -d target_db -tAc "SELECT pgclone.schema('${SOURCE_CONNINFO}', 'ps_coe', true)" 2>&1 || true)
check "default: error raised" "1" "$(echo "$OUT" | grep -c 'COPY completed with error' || true)"
check "default: good table not cloned" "f" "$(q "SELECT to_regclass('ps_coe.good') IS NOT NULL")"

echo "---- TEST 2: on_error=continue ----"
reset_target
OUT=$(q "SELECT pgclone.schema('${SOURCE_CONNINFO}', 'ps_coe', true, '{\"on_error\": \"continue\"}')")
check "continue: result reports failure" "COMPLETED WITH ERRORS: 1 table(s) failed: bad" "$OUT"
check "continue: good rows copied" "2" "$(q "SELECT count(*) FROM ps_coe.good")"
check "continue: good FK present" "1" "$(q "SELECT count(*) FROM pg_constraint WHERE conrelid='ps_coe.good'::regclass AND contype='f'")"

echo "---- TEST 3: database clone aggregates ----"
reset_target
OUT=$(q "SELECT pgclone.database('${SOURCE_CONNINFO}', true, '{\"on_error\": \"continue\"}')")
check "database: result reports failure" "1" "$(echo "$OUT" | grep -c 'COMPLETED WITH ERRORS' || true)"

echo "---- TEST 4: invalid value rejected ----"
OUT=$(psql -U postgres -d target_db -tAc "SELECT pgclone.schema('${SOURCE_CONNINFO}', 'ps_coe', true, '{\"on_error\": \"bogus\"}')" 2>&1 || true)
check "invalid on_error rejected" "1" "$(echo "$OUT" | grep -c 'invalid \"on_error\"' || true)"

src "DROP SCHEMA IF EXISTS ps_coe CASCADE"
psql -U postgres -d target_db -q -c "DROP SCHEMA IF EXISTS ps_coe CASCADE" >/dev/null 2>&1

echo ""
echo "CONTINUE-ON-ERROR TEST RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || { echo "SOME TESTS FAILED"; exit 1; }
echo "ALL CONTINUE-ON-ERROR TESTS PASSED"
