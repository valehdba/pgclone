# PgClone Testing

PgClone has a comprehensive test suite that runs across PostgreSQL versions 14–18 using pgTAP, Docker Compose, and GitHub Actions CI.

## Test Suite Overview

The test suite is organized into three test groups:

### 1. pgTAP Sync Tests (66 tests)

Located in `test/pgclone_test.sql`, these cover core synchronous functionality:

| Group | Tests | What It Covers |
|-------|-------|----------------|
| 1. Extension loading | 3 | Extension installs, version string |
| 2. Table clone (data) | 3 | Clone table with data, verify rows |
| 3. Table rename | 3 | Clone with different target name |
| 4. Structure only | 3 | Clone without data, verify 0 rows |
| 5. Schema clone | 5 | Full schema with tables + verify data |
| 6. Indexes | 2 | Verify indexes are cloned |
| 7. Constraints | 3 | PK, FK verification |
| 8. Views | 2 | Regular views cloned |
| 9. Functions | 2 | Functions cloned with correct signatures |
| 10. Selective columns | 3 | Column filtering, verify column count |
| 11. WHERE filter | 2 | Row filtering, verify filtered count |
| 12. SQL injection | 4 | Semicolon, DROP, INSERT rejection, false-positive safety |
| 13. Email mask | 5 | Email masking, domain preserved, original removed |
| 14. Name mask | 2 | Name replacement with XXXX |
| 15. Null mask | 2 | SSN nullification |
| 16. Hash mask | 2 | MD5 hash, 32-char hex output |
| 17. Constant mask | 2 | Fixed value replacement |
| 18. Combined masks | 3 | Multi-mask + WHERE filter composition |
| 19. Auto-discovery | 6 | Detect email, name, phone, salary, ssn columns |
| 20. Mask in-place | 7 | Clone-then-mask workflow, verify UPDATE masking |

### 2. Database Create Tests

Located in `test/test_database_create.sh` — verifies `pgclone_database_create()` creates a new database, clones data into it, and supports idempotent re-cloning.

### 3. Async Tests

Located in `test/test_async.sh` — covers background worker operations (8 tests):

- TEST 1: `pgclone_table_async` — basic async table clone
- TEST 2: `pgclone_table_async` — with different target name
- TEST 3: `pgclone_schema_async` — sequential mode
- TEST 4: `pgclone_progress` — check progress JSON
- TEST 5: `pgclone_jobs` — list all jobs
- TEST 6: `pgclone_jobs_view` — progress tracking view
- TEST 7: `pgclone_clear_jobs` — cleanup completed/failed jobs
- TEST 8: Worker Pool — parallel schema clone with pool workers (v2.2.0)

---

## Running Tests Locally

### Prerequisites

- Docker and Docker Compose installed
- Repository cloned locally

### Run all tests on a specific PostgreSQL version

```bash
docker compose down -v
docker compose up source-db -d
docker compose run --rm test-pg17
```

### Filter test output

```bash
docker compose run --rm test-pg17 2>&1 | grep -E "(ok|not ok|WARNING|ERROR|PASSED|FAILED)"
```

### Run on all PostgreSQL versions

```bash
for pg in 14 15 16 17 18; do
    echo "=== PostgreSQL $pg ==="
    docker compose down -v
    docker compose up source-db -d
    docker compose run --rm test-pg$pg 2>&1 | tail -5
done
```

### Clean up

```bash
docker compose down -v
```

---

## Docker Test Architecture

### Source Database (`source-db`)

- Uses `postgres:18` image
- Seeded with `test/fixtures/seed.sql` containing test schemas, tables, indexes, constraints, triggers, views, materialized views, functions, and sample data
- Exposed via Docker Compose networking (no host port mapping needed)

### Test Containers (`test-pg14` through `test-pg18`)

Each test container:

1. Builds from the project `Dockerfile` with a `PG_VERSION` build arg
2. Installs build dependencies, pgTAP, and compiles pgclone
3. Configures `shared_preload_libraries = 'pgclone'` and `max_worker_processes = 32`
4. Runs `test/run_tests.sh` as an initdb script, which:
   - Waits for the source database to be ready
   - Creates extensions (pgtap, pgclone)
   - Sets the source connection info as a PostgreSQL GUC (`app.source_conninfo`)
   - Runs pgTAP tests, database create tests, and async tests
   - Reports pass/fail and exits

### Why Docker Compose Instead of Host-Based CI?

The extension installs a `.so` file into PostgreSQL's `pkglibdir`. When the target database runs inside a Docker container but the extension is built on the host, the container can't find the `.so`. Docker Compose ensures the extension is built *inside* the same container that runs the target PostgreSQL, solving this path mismatch.

---

## GitHub Actions CI

The CI pipeline (`.github/workflows/ci.yml`) runs on every push to `main`, `fix/**`, and `feature/**` branches, and on PRs to `main`.

### Matrix Strategy

Tests run in parallel across PostgreSQL 14, 15, 16, 17, and 18 with `fail-fast: false` — so a failure on one version doesn't cancel the others.

### CI Steps

1. **Install PostgreSQL** — from PGDG APT repository
2. **Build pgclone** — `make PG_CONFIG=...` with version-specific `pg_config`
3. **Start target PostgreSQL** — separate instance on port 5434 with `shared_preload_libraries = 'pgclone'`
4. **Seed source database** — GitHub Actions service container on port 5433
5. **Install pgTAP** — built against the same PG version
6. **Run sync tests** — 66 pgTAP tests
7. **Run database_create tests** — creates/clones/verifies/cleans up
8. **Run async tests** — background worker tests with polling
9. **Show logs on failure** — dumps PostgreSQL server log for debugging

### Service Container

The source database runs as a GitHub Actions service container (`postgres:${{ matrix.pg_version }}`), which provides a fresh source database for each matrix job.

---

## Test Data

The test fixture (`test/fixtures/seed.sql`) creates:

- **Schema:** `test_schema`
- **Tables:** `customers` (10 rows), `orders` (10 rows), `order_items` (10 rows)
- **Indexes:** 5 indexes including expression index (`lower(name)`) and DESC index
- **Constraints:** PK on all tables, FK (`orders → customers`, `order_items → orders`), UNIQUE (`email`), CHECK (`score`)
- **Triggers:** 2 triggers with trigger functions
- **Sequences:** `invoice_seq`
- **Views:** `active_customers`, `order_summary`
- **Materialized View:** `customer_stats` with unique index
- **Functions:** `update_timestamp()`, `log_order_change()`, `get_customer_orders(int)`
- **Public schema table:** `simple_test` (5 rows) for basic clone tests

---

## Adding New Tests

### pgTAP test

Add to `test/pgclone_test.sql`, increment the plan count:

```sql
SELECT plan(34);  -- was 33

-- Your new test
SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true)',
        current_setting('app.source_conninfo'),
        'public', 'new_table'),
    'description of what this tests'
);
```

### Shell-based test

Create a new script in `test/` and add it to `test/run_tests.sh`:

```bash
bash /build/pgclone/test/test_your_feature.sh 2>&1
YOUR_EXIT=$?
if [ $YOUR_EXIT -ne 0 ]; then
    TEST_EXIT=1
fi
```
