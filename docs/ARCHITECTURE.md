# PgClone Architecture

This document describes the internal architecture of pgclone, covering the codebase structure, key design decisions, and PostgreSQL version compatibility.

## Codebase Structure

```
pgclone/
├── src/
│   ├── pgclone.c          # Main extension (~3000 lines)
│   │                      #   - Table, schema, database clone functions
│   │                      #   - DDL generation (indexes, constraints, triggers, views)
│   │                      #   - COPY protocol data transfer
│   │                      #   - Selective column / WHERE filter logic
│   │                      #   - _PG_init(), shmem hooks, version function
│   ├── pgclone_bgw.c      # Background worker (~1000 lines)
│   │                      #   - bgw_main entry point
│   │                      #   - Async table/schema clone workers
│   │                      #   - Worker pool (pgclone_pool_worker_main)
│   │                      #   - Shared memory progress updates
│   └── pgclone_bgw.h      # Shared definitions
│                          #   - Job state struct, status enums
│                          #   - Shared memory layout (pgclone_state)
│                          #   - Pool queue struct (PgclonePoolQueue)
│                          #   - MAX_JOBS, MAX_POOL_TASKS, progress fields
├── sql/
│   └── pgclone--X.Y.Z.sql # SQL function definitions per version
├── test/
│   ├── fixtures/seed.sql  # Test data
│   ├── pgclone_test.sql   # 33 pgTAP tests
│   ├── run_tests.sh       # Test orchestrator
│   ├── run_all.sh         # Multi-version runner
│   ├── test_async.sh      # Async test suite (8 tests incl. worker pool)
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

### COPY Protocol Data Transfer

Data is transferred using PostgreSQL's COPY protocol, which is significantly faster than row-by-row INSERT:

1. Open a `COPY ... TO STDOUT` on the source connection
2. Open a `COPY ... FROM STDIN` on the target connection
3. Stream data between them in chunks via `PQgetCopyData` / `PQputCopyData`
4. Finalize with `PQputCopyEnd`

This avoids parsing and re-serializing individual rows.

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
- Read by `pgclone_progress()`, `pgclone_jobs()`, and `pgclone_jobs_view`
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

1. **Registration:** `pgclone_table_async()` or `pgclone_schema_async()` allocates a job slot in shared memory, populates connection info and parameters, then calls `RegisterDynamicBackgroundWorker()`.

2. **Startup:** The worker process starts via `pgclone_bgw_main()`, which:
   - Sets up signal handlers
   - Connects to both source and target databases via libpq
   - Updates job status to RUNNING

3. **Execution:** The worker calls the same core clone functions used by sync operations, with periodic updates to shared memory (rows copied, current table, elapsed time).

4. **Worker Pool mode (v2.2.0):** For `pgclone_schema_async` with `"parallel": N`, the parent process:
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
