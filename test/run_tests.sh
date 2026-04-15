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
    SELECT pgclone.version();
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

# Run loopback-DDL tests (clone_roles, verify, masking_report, DDM)
echo ""
echo "============================================"
echo "Running pgclone loopback-DDL tests..."
echo "============================================"

bash /build/pgclone/test/test_loopback.sh 2>&1

LOOPBACK_EXIT=$?

if [ $LOOPBACK_EXIT -ne 0 ]; then
    TEST_EXIT=1
fi

# Run pgclone.database_create tests (outside transaction)
echo ""
echo "============================================"
echo "Running pgclone.database_create tests..."
echo "============================================"

bash /build/pgclone/test/test_database_create.sh 2>&1

DB_CREATE_EXIT=$?

if [ $DB_CREATE_EXIT -ne 0 ]; then
    TEST_EXIT=1
fi

# Run async tests (require shared_preload_libraries = 'pgclone')
echo ""
echo "============================================"
echo "Running pgclone ASYNC tests..."
echo "============================================"

# Check if pgclone is in shared_preload_libraries
SPL=$(psql -U postgres -d target_db -tAc "SHOW shared_preload_libraries;" 2>/dev/null || echo "")
if echo "$SPL" | grep -q "pgclone"; then
    bash /build/pgclone/test/test_async.sh 2>&1
    ASYNC_EXIT=$?
    if [ $ASYNC_EXIT -ne 0 ]; then
        TEST_EXIT=1
    fi
else
    echo "WARNING: pgclone not in shared_preload_libraries, skipping async tests"
    echo "To run async tests, add to postgresql.conf:"
    echo "  shared_preload_libraries = 'pgclone'"
fi

echo ""
echo "============================================"
if [ $TEST_EXIT -eq 0 ]; then
    echo "ALL TESTS PASSED on $(pg_config --version)"
else
    echo "SOME TESTS FAILED on $(pg_config --version)"
fi
echo "============================================"

# Stop PostgreSQL to force container exit (Docker entrypoint would
# otherwise restart the server after initdb scripts complete)
pg_ctl -D "$PGDATA" -m fast stop 2>/dev/null || true

exit $TEST_EXIT
