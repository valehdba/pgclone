# Changelog

All notable changes to pgclone are documented in this file.

## [2.2.1]

### Added
- **WHERE clause SQL injection protection**: Two-layer defense against malicious input in the `"where"` JSON option
  - Layer 1: Keyword validation rejects DDL/DML keywords (`DROP`, `INSERT`, `UPDATE`, `DELETE`, `CREATE`, `ALTER`, `TRUNCATE`, etc.) and semicolons before the query is sent
  - Layer 2: Source connection runs inside `BEGIN TRANSACTION READ ONLY` when a WHERE clause is present — PostgreSQL itself blocks any write operations even if validation is bypassed
  - Word-boundary-aware matching avoids false positives on column names like `created_at`, `update_count`, `drop_rate`
- 4 new pgTAP tests (37 total): semicolon rejection, DROP keyword rejection, INSERT keyword rejection, false-positive safety with `created_at` column

## [2.2.0]

### Changed
- **Worker Pool Architecture**: Parallel cloning (`"parallel": N`) now uses a fixed-size worker pool instead of spawning one background worker per table
  - Exactly N workers are launched, each pulling tasks from a shared queue
  - Dynamic load balancing: faster workers automatically handle more tables
  - Resource usage reduced from O(tables) to O(N) for bgworkers and DB connections
  - No longer risk exhausting `max_worker_processes` on large schemas

### Added
- `PgclonePoolQueue` struct in shared memory for task queue management
- `pgclone_pool_worker_main()` background worker entry point
- Pool worker test in `test/test_async.sh` (TEST 8)
- `PGCLONE_MAX_POOL_TASKS` limit (512 tables per pool operation)
- Guard against concurrent pool operations

### Removed
- Per-table background worker launch in parallel mode (replaced by pool)
- Per-table job slot allocation in parallel mode (pool workers share fewer slots)

## [2.1.4]

### Changed
- Local loopback connections now use Unix domain sockets (from `unix_socket_directories` GUC) instead of TCP `127.0.0.1`
- `pg_hba.conf` `trust` entry for `127.0.0.1` is no longer required for async operations — default `local all all peer` is sufficient
- Falls back to TCP `127.0.0.1` automatically if Unix sockets are unavailable

### Fixed
- Security: removed unnecessary `trust` authentication requirement for background worker connections

## [2.1.3]

### Fixed
- Async bgworker: COPY pipeline error handling — failures now logged with `PQerrorMessage`, source COPY result consumed on error path to prevent connection leak
- Async bgworker: `WaitForBackgroundWorkerStartup` added to all async functions — jobs no longer stuck in 'pending' state
- `pgclone_schema_async` parallel mode: fixed hardcoded `jobs[0]` write that corrupted slot 0 in shared memory
- `pgclone_schema_async` parallel mode: parent job now correctly transitions to COMPLETED after child workers finish

### Added
- `pgclone_clear_jobs()` function to free completed/cancelled job slots from shared memory
- Async test suite (`test/test_async.sh`) covering `pgclone_table_async`, `pgclone_schema_async`, `pgclone_progress`, `pgclone_jobs_view`, `pgclone_clear_jobs`
- `CONTRIBUTING.md` — development setup, code guidelines, PR process
- `SECURITY.md` — vulnerability reporting, security considerations
- Documentation restructured: `docs/USAGE.md`, `docs/ASYNC.md`, `docs/TESTING.md`, `docs/ARCHITECTURE.md`, `CHANGELOG.md`

## [2.1.2]

### Added
- Elapsed time column in `pgclone_jobs_view`
- `elapsed_time` field in progress detail output

## [2.1.1]

### Changed
- Visual progress bar in `pgclone_jobs_view` replaces verbose NOTICE messages
- Per-table/per-row NOTICE messages moved to DEBUG1 level

## [2.1.0]

### Added
- `pgclone_jobs_view` — query async job progress as a standard PostgreSQL view
- `pgclone_progress_detail()` — table-returning function for detailed progress

## [2.0.1]

### Added
- `pgclone_database_create()` — create a new database and clone into it
- Automatic pgclone extension installation in the target database
- Idempotent behavior: clones into existing database if it already exists

## [2.0.0]

### Added
- True multi-worker parallel cloning with `"parallel": N` option
- Each table gets its own background worker
- Parent worker monitors child workers via shared memory

### Changed
- Shared memory layout expanded for parallel job tracking

## [1.2.0]

### Added
- Materialized view cloning during schema clone (with `"matviews": false` opt-out)
- Exclusion constraint support (cloned alongside PK, UNIQUE, CHECK, FK)
- Materialized view indexes and data are preserved

## [1.1.0]

### Added
- Selective column cloning with `"columns": [...]` JSON option
- Data filtering with `"where": "..."` JSON option
- Automatic filtering of constraints/indexes referencing excluded columns

## [1.0.0]

### Added
- Async clone operations via background workers (`pgclone_table_async`, `pgclone_schema_async`)
- Job progress tracking (`pgclone_progress`, `pgclone_jobs`)
- Job cancellation (`pgclone_cancel`)
- Job resume from checkpoint (`pgclone_resume`)
- Job cleanup (`pgclone_clear_jobs`)
- Conflict resolution strategies: error, skip, replace, rename

## [0.3.0]

### Added
- Background worker infrastructure
- Shared memory allocation for job state
- `_PG_init` with shmem hooks

## [0.2.0]

### Added
- Index cloning (including expression and partial indexes)
- Constraint cloning (PRIMARY KEY, UNIQUE, CHECK, FOREIGN KEY)
- Trigger cloning with trigger functions
- JSON options format for controlling indexes/constraints/triggers
- Boolean parameter variants (`pgclone_table_ex`, `pgclone_schema_ex`)

## [0.1.0]

### Added
- Initial release
- `pgclone_table()` — clone a single table with or without data
- `pgclone_schema()` — clone an entire schema
- `pgclone_functions()` — clone functions only
- `pgclone_database()` — clone all user schemas
- COPY protocol for fast data transfer
- `pgclone_version()` — version string
- Support for PostgreSQL 14–18
- pgTAP test suite (33 tests)
- Docker Compose multi-version test infrastructure
- GitHub Actions CI pipeline
