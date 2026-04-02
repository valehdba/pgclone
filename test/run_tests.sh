#!/bin/bash
# ============================================================
# pgclone test runner
# Runs inside the Docker container after PostgreSQL starts
# ============================================================

set -e

echo "============================================"
echo "pgclone test runner"
echo "PostgreSQL version: $(pg_config --version)"
echo "============================================"

# Wait for source database
echo "Waiting for source database..."
for i in $(seq 1 30); do
    if PGPASSWORD=testpass psql -h source-db -U postgres -d source_db -c "SELECT 1" > /dev/null 2>&1; then
        echo "Source database is ready!"
        break
    fi
    if [ $i -eq 30 ]; then
        echo "ERROR: Source database not ready after 30 seconds"
        exit 1
    fi
    sleep 1
done

# Set source conninfo for tests
export SOURCE_CONNINFO="host=source-db dbname=source_db user=postgres password=testpass"

# Create extensions
echo ""
echo "Installing extensions..."
psql -U postgres -d target_db <<SQL
    CREATE EXTENSION IF NOT EXISTS pgtap;
    CREATE EXTENSION IF NOT EXISTS pgclone;
    SELECT pgclone_version();
SQL

# Set conninfo as a GUC so tests can access it
psql -U postgres -d target_db -c "ALTER DATABASE target_db SET app.source_conninfo = '${SOURCE_CONNINFO}';"

# Run pgTAP tests
echo ""
echo "============================================"
echo "Running pgTAP tests..."
echo "============================================"

# Use psql to run tests (pg_prove may not be installed)
psql -U postgres -d target_db -f /build/pgclone/test/pgclone_test.sql 2>&1

TEST_EXIT=$?

# Run pgclone_database_create tests (outside transaction)
echo ""
echo "============================================"
echo "Running pgclone_database_create tests..."
echo "============================================"

bash /build/pgclone/test/test_database_create.sh 2>&1

DB_CREATE_EXIT=$?

if [ $DB_CREATE_EXIT -ne 0 ]; then
    TEST_EXIT=1
fi

echo ""
echo "============================================"
if [ $TEST_EXIT -eq 0 ]; then
    echo "ALL TESTS PASSED on $(pg_config --version)"
else
    echo "SOME TESTS FAILED on $(pg_config --version)"
fi
echo "============================================"

exit $TEST_EXIT
