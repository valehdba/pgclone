#!/bin/bash
# ============================================================
# MySQL integration test runner (Docker). Runs inside the test-mysql-pg*
# container as an initdb script, after PostgreSQL is up with pgclone loaded.
# Prints "ALL TESTS PASSED" on success so test/run_all.sh can detect it.
# ============================================================
set -uo pipefail

MY_HOST="${MYSQL_HOST:-mysql-db}"
MY_USER="${MYSQL_USER:-root}"
MY_PASSWORD="${MYSQL_PASSWORD:-Pgclone_Test1}"
# ssl=no: the test server uses a self-signed certificate.
export MYSQL_CONNINFO="host=${MY_HOST} port=3306 user=${MY_USER} password=${MY_PASSWORD} ssl=no"
mysql_cli() { mariadb --skip-ssl --default-character-set=utf8mb4 -h "${MY_HOST}" -P 3306 -u "${MY_USER}" -p"${MY_PASSWORD}" "$@"; }

echo "============================================"
echo "pgclone MySQL test runner"
echo "PostgreSQL version: $(pg_config --version)"
echo "============================================"

echo "Waiting for MySQL (first start can take a minute)..."
for i in $(seq 1 60); do
    if mysql_cli -e "SELECT 1" >/dev/null 2>&1; then
        echo "MySQL is ready!"
        break
    fi
    if [ "$i" -eq 60 ]; then
        echo "ERROR: MySQL not ready after 5 minutes"
        mysql_cli -e "SELECT 1" 2>&1 | tail -3
        echo "SOME TESTS FAILED on $(pg_config --version)"
        pg_ctl -D "$PGDATA" -m fast stop 2>/dev/null || true
        exit 1
    fi
    sleep 5
done

psql -U postgres -d target_db -c "CREATE EXTENSION IF NOT EXISTS pgclone;" || true

echo "Seeding MySQL..."
mysql_cli < /build/pgclone/test/fixtures/mysql_seed.sql
SEED_EXIT=$?

TEST_EXIT=$SEED_EXIT
if [ "$SEED_EXIT" -eq 0 ]; then
    PGUSER=postgres PGDATABASE=target_db bash /build/pgclone/test/test_mysql.sh 2>&1
    TEST_EXIT=$?
else
    echo "ERROR: MySQL seed failed"
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
pg_ctl -D "$PGDATA" -m fast -w stop 2>/dev/null || true
exit "$TEST_EXIT"
