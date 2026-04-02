#!/bin/bash
# ============================================================
# pgclone_database_create test
# Tests creating a new database and cloning into it
# Must run outside pgTAP transaction (CREATE DATABASE)
# ============================================================

set -e

echo "============================================"
echo "Testing pgclone_database_create"
echo "============================================"

SOURCE_CONNINFO="host=source-db dbname=source_db user=postgres password=testpass"

# Install pgclone in postgres DB (the admin DB we run from)
echo "Installing pgclone in postgres DB..."
psql -U postgres -d postgres <<SQL
    CREATE EXTENSION IF NOT EXISTS pgclone;
SQL

# Drop clone_test_db if it exists from a previous run
echo "Cleaning up any previous test DB..."
psql -U postgres -d postgres -c "DROP DATABASE IF EXISTS clone_test_db;" 2>/dev/null || true

# ---- TEST 1: Create new database and clone ----
echo ""
echo "TEST 1: pgclone_database_create creates DB and clones"
psql -U postgres -d postgres -v ON_ERROR_STOP=1 <<SQL
    SELECT pgclone_database_create(
        '${SOURCE_CONNINFO}',
        'clone_test_db',
        true
    );
SQL

echo "PASS: pgclone_database_create completed without error"

# ---- TEST 2: Verify database was created ----
DB_EXISTS=$(psql -U postgres -d postgres -tAc "SELECT 1 FROM pg_database WHERE datname = 'clone_test_db'")
if [ "$DB_EXISTS" = "1" ]; then
    echo "PASS: clone_test_db database exists"
else
    echo "FAIL: clone_test_db database was not created"
    exit 1
fi

# ---- TEST 3: Verify schemas were cloned ----
SCHEMA_COUNT=$(psql -U postgres -d clone_test_db -tAc "SELECT count(*) FROM pg_namespace WHERE nspname NOT LIKE 'pg_%' AND nspname <> 'information_schema'")
echo "Schemas found in clone_test_db: $SCHEMA_COUNT"
if [ "$SCHEMA_COUNT" -ge 2 ]; then
    echo "PASS: multiple schemas cloned (public + test_schema)"
else
    echo "FAIL: expected at least 2 schemas, got $SCHEMA_COUNT"
    exit 1
fi

# ---- TEST 4: Verify tables were cloned ----
TABLE_COUNT=$(psql -U postgres -d clone_test_db -tAc "SELECT count(*) FROM pg_tables WHERE schemaname = 'test_schema'")
echo "Tables in test_schema: $TABLE_COUNT"
if [ "$TABLE_COUNT" -ge 3 ]; then
    echo "PASS: tables cloned into test_schema"
else
    echo "FAIL: expected at least 3 tables in test_schema, got $TABLE_COUNT"
    exit 1
fi

# ---- TEST 5: Verify data was cloned ----
ROW_COUNT=$(psql -U postgres -d clone_test_db -tAc "SELECT count(*) FROM test_schema.customers")
echo "Rows in test_schema.customers: $ROW_COUNT"
if [ "$ROW_COUNT" -ge 1 ]; then
    echo "PASS: data cloned into test_schema.customers"
else
    echo "FAIL: no data in test_schema.customers"
    exit 1
fi

# ---- TEST 6: Run again — should work on existing DB ----
echo ""
echo "TEST 6: pgclone_database_create on existing DB (idempotent)"
psql -U postgres -d postgres -v ON_ERROR_STOP=1 <<SQL
    SELECT pgclone_database_create(
        '${SOURCE_CONNINFO}',
        'clone_test_db',
        true
    );
SQL

echo "PASS: pgclone_database_create works on existing database"

# ---- TEST 7: Verify simple_test table in public schema ----
SIMPLE_EXISTS=$(psql -U postgres -d clone_test_db -tAc "SELECT count(*) FROM pg_tables WHERE schemaname = 'public' AND tablename = 'simple_test'")
if [ "$SIMPLE_EXISTS" = "1" ]; then
    echo "PASS: simple_test table exists in public schema"
else
    echo "FAIL: simple_test table not found in public schema"
    exit 1
fi

# Cleanup
echo ""
echo "Cleaning up..."
psql -U postgres -d postgres -c "DROP DATABASE IF EXISTS clone_test_db;" 2>/dev/null || true

echo ""
echo "============================================"
echo "ALL pgclone_database_create TESTS PASSED"
echo "============================================"
