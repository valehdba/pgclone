# PgClone Architecture

This document describes the internal architecture of pgclone, covering the codebase structure, key design decisions, and PostgreSQL version compatibility.

## Codebase Structure

```
pgclone/
├── src/
│   ├── pgclone.c          # Main extension (~5100 lines)
│   │                      #   - Table, schema, database clone functions
│   │                      #   - DDL generation (indexes, constraints, triggers, views)
│   │                      #   - COPY protocol data transfer
│   │                      #   - Selective column / WHERE filter logic
│   │                      #   - Data masking / anonymization engine
│   │                      #   - Auto-discovery of sensitive data
│   │                      #   - Static mask-in-place via UPDATE
│   │                      #   - Dynamic masking policies via views
│   │                      #   - Role cloning with permissions and passwords
│   │                      #   - _PG_init(), shmem hooks, version function
│   ├── pgclone_bgw.c      # Background worker (~1000 lines)
│   │                      #   - bgw_main entry point
│   │                      #   - Async table/schema clone workers
│   │                      #   - Worker pool (pgclone_pool_worker_main)
│   │                      #   - Shared memory progress updates
│   ├── pgclone_bgw.h      # Shared definitions
│   │                      #   - Job state struct, status enums
│   │                      #   - Shared memory layout (pgclone_state)
│   │                      #   - Pool queue struct (PgclonePoolQueue)
│   │                      #   - MAX_JOBS, MAX_POOL_TASKS, progress fields
│   ├── pgclone_diff.c     # v4.1.0 — schema drift / DDL diff (~600 lines)
│   │                      #   - pgclone_diff(): JSON drift report
│   │                      #   - Read-only on both sides; isolated unit
│   │                      #   - Self-contained connect helpers + JSON escaper
│   └── pgclone_preflight.c # v4.2.0 — pre-flight validator (~700 lines)
│                          #   - pgclone_preflight(): JSON readiness report
│                          #   - Checks: connection, version, permissions,
│                          #     capacity, name conflicts, missing
│                          #     extensions/roles/tablespaces
│                          #   - Read-only on both sides; isolated unit
├── sql/
│   └── pgclone--X.Y.Z.sql # SQL function definitions per version
├── test/
│   ├── fixtures/seed.sql  # Test data
│   ├── pgclone_test.sql   # 66 pgTAP tests (groups 1–20)
│   ├── test_loopback.sh   # 21 loopback-DDL tests (roles, verify, report, DDM)
│   ├── run_tests.sh       # Test orchestrator
│   ├── run_all.sh         # Multi-version runner
│   ├── test_async.sh      # Async test suite (21 tests incl. worker pool)
│   └── test_database_create.sh
├── .github/workflows/ci.yml  # GitHub Actions CI (PG 14–18 matrix)
├── Dockerfile             # Multi-version build container
├── docker-compose.yml     # Source + test containers (PG 14–18)
├── Makefile               # PGXS-based build
├── pgclone.control        # Extension metadata
├── META.json              # PGXN metadata
├── pre_deploy_checks.sh   # Pre-release validation (22 checks)
├── CHANGELOG.md           # Version history
├── CONTRIBUTING.md        # Contributor guide
└── SECURITY.md            # Security policy
```

## Core Design Decisions

### Why libpq Instead of SPI?

pgclone uses **loopback libpq connections** to the local target database for all DDL operations instead of PostgreSQL's SPI (Server Programming Interface). The reason: SPI executes within the calling transaction's snapshot, so DDL statements like `CREATE TABLE` aren't visible to subsequent SPI calls within the same function invocation until the transaction commits. By connecting via libpq (even to `localhost`), each DDL statement executes in its own transaction and is immediately visible.

### Why C Instead of PL/pgSQL?

- Direct access to the COPY protocol via `PQgetCopyData` / `PQputCopyData` for high-throughput data transfer
- Background worker registration requires C (`RegisterDynamicBackgroundWorker`)
- Shared memory allocation for progress tracking requires C hooks
- Fine-grained error handling and resource cleanup with `PG_TRY` / `PG_CATCH`

### Isolated Translation Units for Read-Only Features (v4.1.0+)

Read-only feature additions live in their own `.c` files and do **not**
share helpers with `src/pgclone.c`:

- `src/pgclone_diff.c` (v4.1.0) — `pgclone.diff()`
- `src/pgclone_preflight.c` (v4.2.0) — `pgclone.preflight()`

Each such file:

1. **Re-implements** the connect / READ ONLY / `quote_literal_cstr` / JSON
   escape helpers it needs. Duplication is intentional: a bug or
   refactor in `pgclone.c` cannot regress these features, and the
   reverse is also true.
2. **Wraps both source and local connections** in
   `BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY` and ends with
   `ROLLBACK`. The function never executes DDL or DML.
3. **Returns a single JSON document** assembled with `StringInfo`
   appends and a local RFC 8259 string escaper (`pgd_escape_json` /
   `pf_escape_json`) so the feature has zero dependency on
   `utils/jsonapi.h` (whose location varies across PG versions).

When adding the next read-only feature (e.g. `pgclone.table_sample()`
in v4.3.0), follow the same pattern: a new `src/pgclone_<name>.c`,
appended to `OBJS` in the Makefile, with its SQL function declared in
both the per-version full-install file and the upgrade script.

### Consistent-snapshot clones (v4.3.0+)

Every clone wraps source-side reads in a `BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY` transaction. For multi-connection operations — schema clones (per-table sub-calls plus FK retry / views / matviews / functions / triggers passes), database clones, and parallel pool mode — one keeper connection exports a snapshot via `pg_export_snapshot()` and every other source connection imports it via `SET TRANSACTION SNAPSHOT`. This is the correctness model `pg_dump -j` uses for parallel-dump consistency.

Three call patterns:

1. **Single-connection sync (`pgclone.table()`)** — the source connection BEGINs at REPEATABLE READ READ ONLY for the function body, COMMITs at the end. No snapshot export needed because nothing else opens its own source connection.

2. **Multi-connection sync (`pgclone.schema()`, `pgclone.database()`)** — the initial source connection becomes the snapshot keeper. It exports a snapshot ID (or imports one passed via JSON options when nested under another consistent op) and stays open for the full duration of the clone. Every sub-DirectFunctionCall and every later libpq connection (FK retry, views, matviews, functions, triggers) receives the snapshot ID through the options JSON and `SET TRANSACTION SNAPSHOT` on its own source connection.

3. **Parallel pool async (`pgclone.schema_async(... '{"parallel": N}')`)** — a dedicated `pgclone_pool_coordinator_main` background worker is launched first. It opens its own source connection, BEGINs, calls `pg_export_snapshot()`, publishes the ID to shared memory (`PgclonePoolQueue.snapshot_id`) and sets `snapshot_ready = true`. The N pool workers wait on a latch for the flag, import the snapshot, and bump `snapshot_imported_count`. The coordinator COMMITs and exits once `imported_count == snapshot_expected_workers && launch_complete` — at which point every importer's own transaction independently owns the snapshot.

Failure handling: if any pool worker fails to import, it sets `pool.snapshot_failed = true`; the coordinator and every other worker that hasn't yet bound abort cleanly. If the coordinator fails before publishing, workers time out (~60s wait) and report failure. The coordinator caps its hold at ~10 minutes regardless.

Opt-out: `'{"consistent": false}'` in any options-JSON skips all of the above and runs with v4.2.x semantics. The keeper connection is then closed immediately after the table-list read like before, and the pool coordinator is not launched.

### Snapshot-keeper resilience (v4.3.1, issue #9)

The keeper transaction sits idle-in-transaction for the bulk of a long clone. Three independent paths can silently terminate it and invalidate the exported snapshot:

1. **Firewall / NAT idle TCP drop** on remote source connections. pgclone injects `keepalives=1 keepalives_idle=30 keepalives_interval=10 keepalives_count=6` into every source conninfo (unless the user already set them), giving ~90s drop detection and keeping perimeter NAT entries warm. Done at the libpq layer via `PQconninfoParse` + `PQconnectdbParams`, so both URI (`postgresql://...`) and keyword-form conninfo strings are handled.
2. **`idle_in_transaction_session_timeout`** on the source server. The keeper's BEGIN issues `SET LOCAL idle_in_transaction_session_timeout = 0` and `SET LOCAL statement_timeout = 0`; both GUCs are `PGC_USERSET` (no privilege required) and `SET LOCAL` reverts at COMMIT, so the settings never leak into pooled connections.
3. **Silent post-drop continuation.** Between major sub-phases of `pgclone.schema()` (per-table loop, FK retry, views, functions, triggers) and `pgclone.database()` (per-schema loop) pgclone runs a cheap `SELECT 1` ping on the keeper. A failure produces a clear error naming the keeper rather than the misleading `invalid snapshot identifier` that PostgreSQL emits when the exported-snapshot file has been reaped.

Operators who explicitly want to bypass snapshot sharing — for example, on an unreliable link where one of the above is guaranteed to fire — can pass `'{"consistent": false}'` in the options JSON. Each importer then begins its own REPEATABLE READ transaction independently. Cross-table referential consistency is no longer guaranteed, but every per-table copy is still internally consistent.

### COPY Protocol Data Transfer

Data is transferred using PostgreSQL's COPY protocol, which is significantly faster than row-by-row INSERT:

1. Open a `COPY ... TO STDOUT` on the source connection
2. Open a `COPY ... FROM STDIN` on the target connection
3. Stream data between them in chunks via `PQgetCopyData` / `PQputCopyData`
4. Finalize with `PQputCopyEnd`

This avoids parsing and re-serializing individual rows.

### Data Masking Architecture (v3.0.0)

Data masking is implemented as **server-side SQL expressions** injected into the COPY query's SELECT list. Instead of `COPY table TO STDOUT`, pgclone generates:

```sql
COPY (SELECT id,
             CASE WHEN email IS NULL THEN NULL
                  WHEN position('@' in email::text) > 0
                  THEN left(email::text, 1) || '***@' || split_part(email::text, '@', 2)
                  ELSE '***' END AS email,
             'XXXX' AS full_name,
             NULL AS ssn
      FROM schema.table) TO STDOUT
```

This approach has three key benefits:

1. **Zero overhead** — masking happens inside PostgreSQL's query executor on the source, data flows through COPY already masked. No row-by-row C processing.
2. **No dependencies** — all mask expressions use built-in PostgreSQL functions (`left()`, `right()`, `md5()`, `split_part()`, `random()`). No pgcrypto or external libraries.
3. **Composable** — masks work with existing `columns`, `where`, `indexes`, `constraints`, and `triggers` options without special handling.

When masks are specified without an explicit column list, pgclone queries `pg_catalog.pg_attribute` on the source to discover column names, then applies mask expressions to matching columns while passing others through unmodified.

---

## Shared Memory Architecture

Async operations use PostgreSQL shared memory to track job progress:

```c
typedef struct PgcloneJobState {
    int         job_id;
    int         status;          // PENDING, RUNNING, COMPLETED, FAILED, CANCELLED
    char        schema_name[NAMEDATALEN];
    char        table_name[NAMEDATALEN];
    char        current_table[NAMEDATALEN];
    int         tables_total;
    int         tables_completed;
    int64       rows_copied;
    int64       start_time_ms;
    int64       elapsed_ms;
    char        error_message[256];
    // ... more fields
} PgcloneJobState;

typedef struct PgcloneSharedState {
    LWLock     *lock;
    int         num_jobs;
    PgcloneJobState jobs[MAX_JOBS];
} PgcloneSharedState;
```

- Allocated once during `_PG_init()` via shared memory hooks
- Protected by a lightweight lock (`LWLock`) for concurrent access
- Read by `pgclone.progress()`, `pgclone.jobs()`, and `pgclone.jobs_view`
- Written by background workers as they progress

---

## PostgreSQL Version Compatibility

pgclone uses C preprocessor guards to maintain compatibility across PG 14–18:

### Shared Memory Request (PG 15+)

PostgreSQL 15 introduced `shmem_request_hook` — shared memory must be requested during this hook, not directly in `_PG_init()`:

```c
#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;

static void pgclone_shmem_request(void) {
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();
    RequestAddinShmemSpace(sizeof(PgcloneSharedState));
    RequestNamedLWLockTranche("pgclone", 1);
}
#endif
```

In `_PG_init()`:

```c
#if PG_VERSION_NUM >= 150000
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = pgclone_shmem_request;
#else
    RequestAddinShmemSpace(sizeof(PgcloneSharedState));
    RequestNamedLWLockTranche("pgclone", 1);
#endif
```

### Signal Handler (PG 17+)

PostgreSQL 17 removed the `die` signal handler, replacing it with `SignalHandlerForShutdownRequest`:

```c
#if PG_VERSION_NUM >= 170000
    #include "postmaster/interrupt.h"
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
#else
    pqsignal(SIGTERM, die);
#endif
```

### Other Version-Specific Guards

- `d.adsrc` was removed from `pg_attrdef` in PG 12+ — pgclone uses `pg_get_expr()` instead
- `strlcpy` vs `strncpy` for safe string copy across versions
- SQL return type consistency across version-specific `.sql` files

---

## Background Worker Lifecycle

1. **Registration:** `pgclone.table_async()` or `pgclone.schema_async()` allocates a job slot in shared memory, populates connection info and parameters, then calls `RegisterDynamicBackgroundWorker()`.

2. **Startup:** The worker process starts via `pgclone_bgw_main()`, which:
   - Sets up signal handlers
   - Connects to both source and target databases via libpq
   - Updates job status to RUNNING

3. **Execution:** The worker calls the same core clone functions used by sync operations, with periodic updates to shared memory (rows copied, current table, elapsed time).

4. **Worker Pool mode (v2.2.0):** For `pgclone.schema_async` with `"parallel": N`, the parent process:
   - Queries the source for the list of tables
   - Populates a shared-memory task queue (`PgclonePoolQueue`)
   - Launches exactly N pool workers via `pgclone_pool_worker_main()`
   - Each worker grabs the next unclaimed task from the queue, clones it, then grabs the next — until the queue is empty
   - Dynamic load balancing: faster workers automatically handle more tables
   - Resource usage is O(N) instead of O(tables) for bgworkers and DB connections

5. **Completion:** Worker sets status to COMPLETED or FAILED, disconnects from databases, and exits.

---

## Local Loopback Connections (v2.1.4+)

pgclone uses loopback libpq connections for DDL execution on the target database. Since v2.1.4, these connections prefer Unix domain sockets (read from the `unix_socket_directories` GUC) over TCP `127.0.0.1`. This means the default `local all all peer` line in `pg_hba.conf` is sufficient — no `trust` entry is needed. If Unix sockets are unavailable, pgclone falls back to TCP `127.0.0.1` automatically.

---

## Resource Management

pgclone carefully manages resources to avoid leaks:

- Every `PQconnectdb()` has a matching `PQfinish()` in all code paths (including error paths)
- Every `PQexec()` result is freed with `PQclear()`
- `PG_TRY / PG_CATCH` blocks ensure cleanup on errors
- Background workers disconnect from both source and target databases before exiting
- COPY pipeline errors consume remaining results to prevent connection state corruption

---

## Build System

pgclone uses PostgreSQL's PGXS build system:

```makefile
MODULES = pgclone
EXTENSION = pgclone
DATA = sql/pgclone--*.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
```

This integrates with `pg_config` to find the correct include paths, library directories, and installation locations for the target PostgreSQL version.
