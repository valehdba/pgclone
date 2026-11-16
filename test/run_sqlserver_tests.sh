#!/bin/bash
# ============================================================
# SQL Server integration test runner (Docker). Runs inside the
# test-sqlserver-pg* container as an initdb script, after PostgreSQL is up with
# pgclone loaded. Prints "ALL TESTS PASSED" on success so test/run_all.sh can
# detect it.
# ============================================================
set -uo pipefail

SS_HOST="${SQLSERVER_HOST:-sqlserver-db}"
SS_USER="${SQLSERVER_USER:-sa}"
SS_PASSWORD="${SQLSERVER_PASSWORD:-Pgclone_Test1}"
export SQLSERVER_CONNINFO="host=${SS_HOST} port=1433 database=pgc_test user=${SS_USER} password=${SS_PASSWORD}"

echo "============================================"
echo "pgclone SQL Server test runner"
echo "PostgreSQL version: $(pg_config --version)"
echo "============================================"

echo "Waiting for SQL Server (first start can take a minute or two)..."
for i in $(seq 1 60); do
    if echo "SELECT 1
go" | tsql -S "${SS_HOST}" -p 1433 -U "${SS_USER}" -P "${SS_PASSWORD}" 2>&1 | grep -q "^1"; then
        echo "SQL Server is ready!"
        break
    fi
    if [ "$i" -eq 60 ]; then
        echo "ERROR: SQL Server not ready after 10 minutes"
        echo "SOME TESTS FAILED on $(pg_config --version)"
        pg_ctl -D "$PGDATA" -m fast stop 2>/dev/null || true
        exit 1
    fi
    sleep 10
done

psql -U postgres -d target_db -c "CREATE EXTENSION IF NOT EXISTS pgclone;" || true

echo "Seeding SQL Server..."
SEED_LOG=$(mktemp)
tsql -S "${SS_HOST}" -p 1433 -U "${SS_USER}" -P "${SS_PASSWORD}" \
    < /build/pgclone/test/fixtures/sqlserver_seed.sql > "$SEED_LOG" 2>&1
cat "$SEED_LOG"
# tsql exits 0 even when a batch fails; look for server error messages.
if grep -q -E "^Msg [0-9]+, Level (1[1-9]|2[0-9])" "$SEED_LOG"; then
    SEED_EXIT=1
else
    SEED_EXIT=0
fi

TEST_EXIT=$SEED_EXIT
if [ "$SEED_EXIT" -eq 0 ]; then
    PGUSER=postgres PGDATABASE=target_db bash /build/pgclone/test/test_sqlserver.sh 2>&1
    TEST_EXIT=$?
else
    echo "ERROR: SQL Server seed failed"
fi

echo ""
echo "============================================"
if [ "$TEST_EXIT" -eq 0 ]; then
    echo "ALL TESTS PASSED on $(pg_config --version)"
else
    echo "SOME TESTS FAILED on $(pg_config --version)"
fi
echo "============================================"

# Stop PostgreSQL so the container exits (the entrypoint would otherwise
# start the server after the initdb scripts finish).
pg_ctl -D "$PGDATA" -m fast stop 2>/dev/null || true
exit "$TEST_EXIT"
