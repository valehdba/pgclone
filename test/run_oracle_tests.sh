#!/bin/bash
# ============================================================
# Oracle integration test runner (Docker). Runs inside the test-oracle-pg*
# container as an initdb script, after PostgreSQL is up with pgclone loaded.
# Prints "ALL TESTS PASSED" on success so test/run_all.sh can detect it.
# ============================================================
set -uo pipefail

ORA_HOST="${ORACLE_HOST:-oracle-db}"
ORA_SERVICE="${ORACLE_SERVICE:-FREEPDB1}"
ORA_USER="${ORACLE_APP_USER:-app}"
ORA_PASSWORD="${ORACLE_APP_PASSWORD:-apppass}"
export ORACLE_CONNINFO="host=${ORA_HOST} port=1521 service=${ORA_SERVICE} user=${ORA_USER} password=${ORA_PASSWORD}"
export NLS_LANG=.AL32UTF8

echo "============================================"
echo "pgclone Oracle test runner"
echo "PostgreSQL version: $(pg_config --version)"
echo "============================================"

echo "Waiting for Oracle (first start can take a few minutes)..."
for i in $(seq 1 90); do
    if echo "SELECT 1 FROM dual;" | sqlplus -L -S "${ORA_USER}/${ORA_PASSWORD}@//${ORA_HOST}:1521/${ORA_SERVICE}" >/dev/null 2>&1; then
        echo "Oracle is ready!"
        break
    fi
    if [ "$i" -eq 90 ]; then
        echo "ERROR: Oracle not ready after 15 minutes"
        echo "SOME TESTS FAILED on $(pg_config --version)"
        pg_ctl -D "$PGDATA" -m fast stop 2>/dev/null || true
        exit 1
    fi
    sleep 10
done

psql -U postgres -d target_db -c "CREATE EXTENSION IF NOT EXISTS pgclone;" || true

echo "Seeding Oracle..."
sqlplus -L -S "${ORA_USER}/${ORA_PASSWORD}@//${ORA_HOST}:1521/${ORA_SERVICE}" @/build/pgclone/test/fixtures/oracle_seed.sql
SEED_EXIT=$?

TEST_EXIT=$SEED_EXIT
if [ "$SEED_EXIT" -eq 0 ]; then
    PGUSER=postgres PGDATABASE=target_db bash /build/pgclone/test/test_oracle.sh 2>&1
    TEST_EXIT=$?
else
    echo "ERROR: Oracle seed failed"
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
