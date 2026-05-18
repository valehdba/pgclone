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
echo "Testing pgclone snapshot-keeper resilience (#9)"
echo "  group 1: idle_in_transaction_session_timeout"
echo "  group 2: statement_timeout"
echo "  group 3: pgclone.database_create keeper"
echo "  group 4: conninfo form handling"
echo "  group 5: async path (v4.3.2 bgw)"
echo "  group 6: transaction_timeout (PG17+)"
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
ALTER ROLE $SOURCE_USER RESET statement_timeout;
ALTER ROLE $SOURCE_USER RESET transaction_timeout;
DROP SCHEMA IF EXISTS keeper_test  CASCADE;
DROP SCHEMA IF EXISTS keeper_test2 CASCADE;
DROP SCHEMA IF EXISTS keeper_probe CASCADE;
DROP SCHEMA IF EXISTS keeper_async CASCADE;
DROP SCHEMA IF EXISTS keeper_txnto CASCADE;
SQL
    tgt "DROP SCHEMA IF EXISTS keeper_test  CASCADE" >/dev/null 2>&1 || true
    tgt "DROP SCHEMA IF EXISTS keeper_test2 CASCADE" >/dev/null 2>&1 || true
    tgt "DROP SCHEMA IF EXISTS keeper_probe CASCADE" >/dev/null 2>&1 || true
    tgt "DROP SCHEMA IF EXISTS keeper_async CASCADE" >/dev/null 2>&1 || true
    tgt "DROP SCHEMA IF EXISTS keeper_txnto CASCADE" >/dev/null 2>&1 || true
    psql -U postgres -d postgres -c "DROP DATABASE IF EXISTS keeper_test_dbc" \
        >/dev/null 2>&1 || true
}
trap cleanup EXIT

# ---- Clean target schema if a previous run left it ----
tgt "DROP SCHEMA IF EXISTS keeper_test CASCADE" >/dev/null

# ---- Run the schema clone ----
echo ""
echo "============================================"
echo "Test group 1: idle_in_transaction_session_timeout"
echo "============================================"
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

# ============================================================
# Test group 2 — statement_timeout variant (issue #9, layer 2)
#
# Verifies that SET LOCAL statement_timeout = 0 on the keeper
# transaction defeats a non-zero source-side statement_timeout.
# A small statement_timeout is more interesting than it sounds:
# COPY of the larger tables, and the keeper's eventual COMMIT,
# both run as statements and would be aborted without the fix.
# ============================================================

echo ""
echo "============================================"
echo "Test group 2: statement_timeout protection"
echo "============================================"

src <<SQL
ALTER ROLE $SOURCE_USER RESET idle_in_transaction_session_timeout;
ALTER ROLE $SOURCE_USER SET statement_timeout = '1s';
SQL

# Re-seed (the earlier trap-cleanup is registered for the FINAL
# exit; we just need a clean schema here)
src <<'SQL'
DROP SCHEMA IF EXISTS keeper_test CASCADE;
CREATE SCHEMA keeper_test;
CREATE TABLE keeper_test.t1 (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_test.t2 (id int PRIMARY KEY, payload text);
INSERT INTO keeper_test.t1 SELECT g, repeat('x', 200) FROM generate_series(1, 5000) g;
INSERT INTO keeper_test.t2 SELECT g, repeat('y', 200) FROM generate_series(1, 5000) g;
SQL

tgt "DROP SCHEMA IF EXISTS keeper_test CASCADE" >/dev/null

CLONE_RC=0
CLONE_OUT=$(tgt "SELECT pgclone.schema('$CONNINFO', 'keeper_test', true)" 2>&1) || CLONE_RC=$?

run_test "pgclone.schema returns OK under tight statement_timeout" \
    "[ '$CLONE_RC' = '0' ] && [ '$CLONE_OUT' = 'OK' ]"

run_test "no 'canceling statement due to statement timeout' error" \
    "! echo '$CLONE_OUT' | grep -qi 'canceling statement'"

for tbl in t1 t2; do
    n=$(tgt "SELECT count(*) FROM keeper_test.$tbl" 2>/dev/null || echo "missing")
    run_test "keeper_test.$tbl was cloned with 5000 rows (actual: $n)" \
        "[ '$n' = '5000' ]"
done

# Clean for next group
src <<SQL
ALTER ROLE $SOURCE_USER RESET statement_timeout;
SQL

# ============================================================
# Test group 3 — pgclone.database_create() keeper resilience
#
# pgclone.database() owns the keeper across an outer per-schema
# loop in addition to the per-table loop inside each schema.
# This group exercises that outer-loop ping site and verifies
# the same SET LOCAL idle_in_transaction_session_timeout=0
# protection works on the database-level wrapper.
# ============================================================

echo ""
echo "============================================"
echo "Test group 3: pgclone.database_create keeper"
echo "============================================"

# Build a TWO-schema source so the per-schema loop is exercised.
src <<'SQL'
DROP SCHEMA IF EXISTS keeper_test  CASCADE;
DROP SCHEMA IF EXISTS keeper_test2 CASCADE;
CREATE SCHEMA keeper_test;
CREATE SCHEMA keeper_test2;
CREATE TABLE keeper_test.a  (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_test.b  (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_test2.c (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_test2.d (id int PRIMARY KEY, payload text);
INSERT INTO keeper_test.a  SELECT g, 'a' FROM generate_series(1, 1000) g;
INSERT INTO keeper_test.b  SELECT g, 'b' FROM generate_series(1, 1000) g;
INSERT INTO keeper_test2.c SELECT g, 'c' FROM generate_series(1, 1000) g;
INSERT INTO keeper_test2.d SELECT g, 'd' FROM generate_series(1, 1000) g;
SQL

src <<SQL
ALTER ROLE $SOURCE_USER SET idle_in_transaction_session_timeout = '1s';
SQL

# Drop any prior target DB; pgclone.database_create creates it fresh.
DBC_NAME="keeper_test_dbc"
psql -U postgres -d postgres -c "DROP DATABASE IF EXISTS $DBC_NAME" >/dev/null 2>&1 || true

DBC_RC=0
DBC_OUT=$(psql -U postgres -d postgres -tAc \
    "SELECT pgclone.database_create('$CONNINFO', '$DBC_NAME', true)" 2>&1) || DBC_RC=$?

run_test "pgclone.database_create returns OK under tight idle_in_transaction_session_timeout" \
    "[ '$DBC_RC' = '0' ] && echo '$DBC_OUT' | grep -q 'OK'"

run_test "no 'invalid snapshot identifier' error in database_create output" \
    "! echo '$DBC_OUT' | grep -qi 'invalid snapshot identifier'"

# Row counts across both schemas
for s_t in "keeper_test.a" "keeper_test.b" "keeper_test2.c" "keeper_test2.d"; do
    n=$(psql -U postgres -d "$DBC_NAME" -tAc "SELECT count(*) FROM $s_t" 2>/dev/null \
        | tr -d '[:space:]' || echo "missing")
    run_test "$DBC_NAME:$s_t has 1000 rows (actual: $n)" "[ '$n' = '1000' ]"
done

# Cleanup the temp DB and the second schema
psql -U postgres -d postgres -c "DROP DATABASE IF EXISTS $DBC_NAME" >/dev/null 2>&1 || true
src <<SQL
ALTER ROLE $SOURCE_USER RESET idle_in_transaction_session_timeout;
DROP SCHEMA IF EXISTS keeper_test2 CASCADE;
SQL

# ============================================================
# Test group 4 — keepalive injection probe (issue #9, layer 1)
#
# v4.3.1 injects TCP keepalives into every source conninfo via
# PQconninfoParse + PQconnectdbParams. Direct verification of
# the resulting SO_KEEPALIVE / TCP_KEEPIDLE on a remote backend
# fd from SQL is not portable, but we CAN verify the injection
# logic at the libpq layer: pgclone reaches the source via
# pgclone_connect_with_keepalives(), which must succeed against
# both URI (postgresql://) and keyword-form conninfo strings.
# Both forms are exercised below; a regression that breaks the
# augmentation (or the PQconninfoParse path entirely) would
# surface here as a connection or clone failure.
# ============================================================

echo ""
echo "============================================"
echo "Test group 4: conninfo form handling"
echo "============================================"

# Build a tiny source schema we can clone twice quickly.
src <<'SQL'
DROP SCHEMA IF EXISTS keeper_probe CASCADE;
CREATE SCHEMA keeper_probe;
CREATE TABLE keeper_probe.t (id int PRIMARY KEY);
INSERT INTO keeper_probe.t SELECT generate_series(1, 100);
SQL

# 4a. keyword-form conninfo (already exercised above but
#     re-asserted as the explicit baseline for this group).
tgt "DROP SCHEMA IF EXISTS keeper_probe CASCADE" >/dev/null
KW_RC=0
KW_OUT=$(tgt "SELECT pgclone.schema('$CONNINFO', 'keeper_probe', true)" 2>&1) || KW_RC=$?
run_test "keyword-form conninfo: clone returns OK" \
    "[ '$KW_RC' = '0' ] && [ '$KW_OUT' = 'OK' ]"
run_test "keyword-form conninfo: 100 rows cloned" \
    "[ \"\$(tgt 'SELECT count(*) FROM keeper_probe.t')\" = '100' ]"

# 4b. URI-form conninfo — the form the issue #9 reporter used
#     and the one most likely to trip up a naive
#     'append " key=val"' implementation.
URI_CONNINFO="postgresql://$SOURCE_USER:$SOURCE_PW@$SOURCE_HOST:$SOURCE_PORT/$SOURCE_DB"
tgt "DROP SCHEMA IF EXISTS keeper_probe CASCADE" >/dev/null
URI_RC=0
URI_OUT=$(tgt "SELECT pgclone.schema('$URI_CONNINFO', 'keeper_probe', true)" 2>&1) || URI_RC=$?
run_test "URI-form conninfo: clone returns OK" \
    "[ '$URI_RC' = '0' ] && [ '$URI_OUT' = 'OK' ]"
run_test "URI-form conninfo: 100 rows cloned" \
    "[ \"\$(tgt 'SELECT count(*) FROM keeper_probe.t')\" = '100' ]"

# 4c. User-supplied explicit keepalive params must be preserved
#     (PQconninfoParse merge logic — the per-keyword check must
#     skip injection when the user already set that keyword).
#     Pass keepalives_idle=120; the clone must still work.
EXPLICIT_CONNINFO="$CONNINFO keepalives_idle=120 keepalives_count=3"
tgt "DROP SCHEMA IF EXISTS keeper_probe CASCADE" >/dev/null
EX_RC=0
EX_OUT=$(tgt "SELECT pgclone.schema('$EXPLICIT_CONNINFO', 'keeper_probe', true)" 2>&1) || EX_RC=$?
run_test "user-supplied keepalive params are accepted and clone succeeds" \
    "[ '$EX_RC' = '0' ] && [ '$EX_OUT' = 'OK' ]"

src <<'SQL'
DROP SCHEMA IF EXISTS keeper_probe CASCADE;
SQL
tgt "DROP SCHEMA IF EXISTS keeper_probe CASCADE" >/dev/null 2>&1 || true

# ============================================================
# Test group 5 — async path keeper resilience (v4.3.2, issue #9 bgw)
#
# v4.3.1 fixed the synchronous keeper. v4.3.2 ports the same
# four-layer fix to src/pgclone_bgw.c so async clones over
# networked sources also survive a non-zero source-side
# idle_in_transaction_session_timeout. This group sets the
# timeout to 1s and runs pgclone.schema_async(...). Without the
# v4.3.2 bgw fix, the single-job worker's source connection or
# the pool coordinator's keeper would be killed and the async
# clone would fail.
#
# Requires pgclone in shared_preload_libraries (provided by CI).
# ============================================================

echo ""
echo "============================================"
echo "Test group 5: async path keeper resilience"
echo "============================================"

# Skip the async group when pgclone isn't preloaded (Docker local
# runs may not have shared_preload_libraries set, in which case
# pgclone.schema_async itself would error out before we test the
# keeper-resilience aspect).
PRELOAD=$(tgt "SHOW shared_preload_libraries" 2>/dev/null || true)
if ! echo "$PRELOAD" | grep -q pgclone; then
    echo "  SKIP: pgclone not in shared_preload_libraries (async group)"
else
    src <<'SQL'
DROP SCHEMA IF EXISTS keeper_async CASCADE;
CREATE SCHEMA keeper_async;
CREATE TABLE keeper_async.t1 (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_async.t2 (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_async.t3 (id int PRIMARY KEY, payload text);
INSERT INTO keeper_async.t1 SELECT g, repeat('x', 200) FROM generate_series(1, 3000) g;
INSERT INTO keeper_async.t2 SELECT g, repeat('y', 200) FROM generate_series(1, 3000) g;
INSERT INTO keeper_async.t3 SELECT g, repeat('z', 200) FROM generate_series(1, 3000) g;
SQL

    src <<SQL
ALTER ROLE $SOURCE_USER SET idle_in_transaction_session_timeout = '1s';
SQL

    tgt "DROP SCHEMA IF EXISTS keeper_async CASCADE" >/dev/null

    # Kick off async schema clone (sequential mode — the v4.3.2 fix
    # applies to both sequential and parallel pool paths via the
    # same bgw_begin_repeatable_read / bgw_connect_with_keepalives
    # helpers).
    JOB_ID=$(tgt "SELECT pgclone.schema_async('$CONNINFO', 'keeper_async', true)" 2>/dev/null \
        | tr -d '[:space:]')
    run_test "schema_async returns a job_id (got: '$JOB_ID')" \
        "[ -n '$JOB_ID' ] && [ '$JOB_ID' != '' ]"

    # Poll for completion — up to ~30 s
    ASYNC_FINAL=""
    for attempt in $(seq 1 60); do
        ASYNC_STATUS=$(tgt "SELECT status FROM pgclone.jobs_view WHERE job_id = $JOB_ID" 2>/dev/null \
            | tr -d '[:space:]')
        if [ "$ASYNC_STATUS" = "completed" ] || [ "$ASYNC_STATUS" = "failed" ]; then
            ASYNC_FINAL="$ASYNC_STATUS"
            break
        fi
        sleep 0.5
    done

    run_test "async clone reached completed status (got: '$ASYNC_FINAL')" \
        "[ '$ASYNC_FINAL' = 'completed' ]"

    # Row counts on the target
    for tbl in t1 t2 t3; do
        n=$(tgt "SELECT count(*) FROM keeper_async.$tbl" 2>/dev/null || echo missing)
        run_test "keeper_async.$tbl cloned with 3000 rows (actual: $n)" \
            "[ '$n' = '3000' ]"
    done

    # Cleanup
    src <<SQL
ALTER ROLE $SOURCE_USER RESET idle_in_transaction_session_timeout;
DROP SCHEMA IF EXISTS keeper_async CASCADE;
SQL
    tgt "DROP SCHEMA IF EXISTS keeper_async CASCADE" >/dev/null 2>&1 || true
fi

echo ""
echo "============================================"
echo "Test group 6: transaction_timeout protection (PG17+)"
echo "============================================"

# ============================================================
# transaction_timeout (PG 17+) caps a transaction's total age and
# fires even on an actively-used keeper, so the idle_in_transaction
# and keepalive mitigations from groups 1-5 do NOT cover it. This
# group verifies the keeper's SET LOCAL transaction_timeout = 0
# defeats a non-zero source-side transaction_timeout. Skipped on
# source servers < 17, where the GUC does not exist (SET would
# raise "unrecognized configuration parameter"). (issue #5)
# ============================================================

SRC_VER=$(src -tAc "SHOW server_version_num" 2>/dev/null | tr -d '[:space:]')
if [ -z "$SRC_VER" ] || [ "$SRC_VER" -lt 170000 ]; then
    echo "  SKIP: source server_version_num='$SRC_VER' (< 170000, no transaction_timeout)"
else
    # Build a dedicated source schema. Earlier groups rebuild
    # keeper_test with different tables, so this group must own its
    # own schema. 5 x 5000 rows takes several seconds to clone —
    # comfortably longer than the 1s transaction_timeout below, so
    # without the fix the keeper transaction is force-terminated and
    # a later per-table importer fails with "invalid snapshot
    # identifier".
    src <<'SQL'
DROP SCHEMA IF EXISTS keeper_txnto CASCADE;
CREATE SCHEMA keeper_txnto;
CREATE TABLE keeper_txnto.t1 (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_txnto.t2 (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_txnto.t3 (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_txnto.t4 (id int PRIMARY KEY, payload text);
CREATE TABLE keeper_txnto.t5 (id int PRIMARY KEY, payload text);
INSERT INTO keeper_txnto.t1 SELECT g, repeat('x', 200) FROM generate_series(1, 5000) g;
INSERT INTO keeper_txnto.t2 SELECT g, repeat('y', 200) FROM generate_series(1, 5000) g;
INSERT INTO keeper_txnto.t3 SELECT g, repeat('z', 200) FROM generate_series(1, 5000) g;
INSERT INTO keeper_txnto.t4 SELECT g, repeat('w', 200) FROM generate_series(1, 5000) g;
INSERT INTO keeper_txnto.t5 SELECT g, repeat('v', 200) FROM generate_series(1, 5000) g;
SQL

    src <<SQL
ALTER ROLE $SOURCE_USER RESET idle_in_transaction_session_timeout;
ALTER ROLE $SOURCE_USER RESET statement_timeout;
ALTER ROLE $SOURCE_USER SET transaction_timeout = '1s';
SQL

    tgt "DROP SCHEMA IF EXISTS keeper_txnto CASCADE" >/dev/null

    CLONE_RC=0
    CLONE_OUT=$(tgt "SELECT pgclone.schema('$CONNINFO', 'keeper_txnto', true)" 2>&1) || CLONE_RC=$?
    echo "  exit code: $CLONE_RC"

    run_test "pgclone.schema returns OK under tight transaction_timeout" \
        "[ '$CLONE_RC' = '0' ] && [ '$CLONE_OUT' = 'OK' ]"

    run_test "no 'invalid snapshot identifier' error under transaction_timeout" \
        "! echo '$CLONE_OUT' | grep -qi 'invalid snapshot identifier'"

    for tbl in t1 t2 t3 t4 t5; do
        n=$(tgt "SELECT count(*) FROM keeper_txnto.$tbl" 2>/dev/null || echo "missing")
        run_test "keeper_txnto.$tbl cloned with 5000 rows under transaction_timeout (actual: $n)" \
            "[ '$n' = '5000' ]"
    done

    src <<SQL
ALTER ROLE $SOURCE_USER RESET transaction_timeout;
DROP SCHEMA IF EXISTS keeper_txnto CASCADE;
SQL
    tgt "DROP SCHEMA IF EXISTS keeper_txnto CASCADE" >/dev/null 2>&1 || true
fi

echo ""
echo "============================================"
echo "Results: $PASS passed, $FAIL failed"
echo "============================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
