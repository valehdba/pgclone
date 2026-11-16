#!/bin/bash
# ============================================================
# Run pgclone tests locally using Docker
#
# Usage:
#   ./test/run_all.sh              # Test on all PG versions
#   ./test/run_all.sh 18           # Test on PG 18 only
#   ./test/run_all.sh 16 17        # Test on PG 16 and 17
#   ./test/run_all.sh oracle       # Oracle -> PostgreSQL tests (PG 14, 17, 18)
#   ./test/run_all.sh oracle 17    # Oracle tests on PG 17 only
#   ./test/run_all.sh sqlserver    # SQL Server -> PostgreSQL tests (PG 14, 17, 18)
#   ./test/run_all.sh sqlserver 17 # SQL Server tests on PG 17 only
#   ./test/run_all.sh mysql        # MySQL -> PostgreSQL tests (PG 14, 17, 18)
#   ./test/run_all.sh mysql 17     # MySQL tests on PG 17 only
# ============================================================

set -e

# "oracle" / "sqlserver" / "mysql" as the first argument switch to the Oracle /
# SQL Server / MySQL -> PostgreSQL suites (need the oracle-db / sqlserver-db /
# mysql-db service; see Dockerfile.oracle, Dockerfile.sqlserver, Dockerfile.mysql).
MODE="pg"
if [ "${1:-}" = "oracle" ]; then
    MODE="oracle"
    shift
    DEFAULT_VERSIONS="14 17 18"
    DB_SERVICE="oracle-db"
    SERVICE_PREFIX="test-oracle-pg"
elif [ "${1:-}" = "sqlserver" ]; then
    MODE="sqlserver"
    shift
    DEFAULT_VERSIONS="14 17 18"
    DB_SERVICE="sqlserver-db"
    SERVICE_PREFIX="test-sqlserver-pg"
elif [ "${1:-}" = "mysql" ]; then
    MODE="mysql"
    shift
    DEFAULT_VERSIONS="14 17 18"
    DB_SERVICE="mysql-db"
    SERVICE_PREFIX="test-mysql-pg"
else
    DEFAULT_VERSIONS="14 15 16 17 18"
    DB_SERVICE="source-db"
    SERVICE_PREFIX="test-pg"
fi
VERSIONS="${@:-$DEFAULT_VERSIONS}"

echo "============================================"
echo "pgclone multi-version test suite"
echo "Testing on PostgreSQL: $VERSIONS (suite: $MODE)"
echo "============================================"
echo ""

# Build source database
echo "Starting $DB_SERVICE..."
docker compose up -d "$DB_SERVICE"
sleep 3

PASS=0
FAIL=0
RESULTS=""

for ver in $VERSIONS; do
    echo ""
    echo "============================================"
    echo "Testing on PostgreSQL $ver..."
    echo "============================================"

    SERVICE="${SERVICE_PREFIX}${ver}"

    if docker compose build "$SERVICE" 2>&1 | tail -5; then
        TMPLOG=$(mktemp)
        docker compose run --rm "$SERVICE" 2>&1 | tee "$TMPLOG" || true
        if grep -q "ALL TESTS PASSED" "$TMPLOG"; then
            echo "✅ PostgreSQL $ver: PASSED"
            RESULTS="$RESULTS\n  ✅ PostgreSQL $ver: PASSED"
            PASS=$((PASS + 1))
        else
            echo "❌ PostgreSQL $ver: FAILED"
            RESULTS="$RESULTS\n  ❌ PostgreSQL $ver: FAILED"
            FAIL=$((FAIL + 1))
        fi
        rm -f "$TMPLOG"
    else
        echo "❌ PostgreSQL $ver: BUILD FAILED"
        RESULTS="$RESULTS\n  ❌ PostgreSQL $ver: BUILD FAILED"
        FAIL=$((FAIL + 1))
    fi
done

# Cleanup
echo ""
echo "Cleaning up..."
docker compose down -v 2>/dev/null

echo ""
echo "============================================"
echo "RESULTS SUMMARY"
echo "============================================"
echo -e "$RESULTS"
echo ""
echo "Passed: $PASS / $((PASS + FAIL))"

if [ $FAIL -gt 0 ]; then
    echo "STATUS: SOME TESTS FAILED"
    exit 1
else
    echo "STATUS: ALL TESTS PASSED ✅"
    exit 0
fi
