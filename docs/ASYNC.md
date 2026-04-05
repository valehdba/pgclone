# PgClone Async Operations

Async functions run clone operations in PostgreSQL background workers, allowing you to continue using your session while cloning proceeds in the background.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Async Table Clone](#async-table-clone)
- [Async Schema Clone](#async-schema-clone)
- [Parallel Cloning](#parallel-cloning-v200)
- [Progress Tracking](#progress-tracking)
- [Progress Tracking View](#progress-tracking-view-v210)
- [Job Management](#job-management)
- [Conflict Resolution in Async Mode](#conflict-resolution-in-async-mode)
- [How It Works](#how-it-works)

## Prerequisites

Add to `postgresql.conf` and restart PostgreSQL:

```
shared_preload_libraries = 'pgclone'
max_worker_processes = 32       # recommended for parallel cloning
```

Without `shared_preload_libraries`, async functions will not be available.

---

## Async Table Clone

```sql
-- Returns a job_id (integer)
SELECT pgclone_table_async(
    'host=source-server dbname=mydb user=postgres',
    'public', 'large_table', true
);
-- Returns: 1
```

All options available for `pgclone_table` also work with `pgclone_table_async`, including target name, JSON options, conflict strategy, selective columns, and WHERE filters.

---

## Async Schema Clone

```sql
SELECT pgclone_schema_async(
    'host=source-server dbname=mydb user=postgres',
    'sales', true
);
```

---

## Parallel Cloning (v2.2.0 — Worker Pool)

Clone tables in parallel using a fixed-size worker pool. Instead of spawning one background worker per table (which could exhaust `max_worker_processes`), pgclone launches exactly N workers that pull tasks from a shared queue:

```sql
-- Clone schema with 4 pool workers
SELECT pgclone_schema_async(
    'host=source-server dbname=mydb user=postgres',
    'sales', true,
    '{"parallel": 4}'
);

-- Combine parallel with other options
SELECT pgclone_schema_async(
    'host=source-server dbname=mydb user=postgres',
    'sales', true,
    '{"parallel": 8, "conflict": "replace", "triggers": false}'
);
```

### How the Worker Pool Works

1. The parent process queries the source for the table list and populates a shared-memory task queue.
2. Exactly N background workers are launched (where N = `"parallel"` value, capped at table count).
3. Each worker grabs the next unclaimed table from the queue, clones it, then grabs the next — until the queue is empty.
4. Faster workers automatically handle more tables (dynamic load balancing).
5. The parent job tracks aggregate progress across all workers.

### Benefits over v2.0.0

| Aspect | v2.0.0 (per-table workers) | v2.2.0 (worker pool) |
|--------|---------------------------|---------------------|
| 100 tables, parallel=4 | 100 bgworkers | **4 bgworkers** |
| DB connections | 200 (100×2) | **8** (4×2) |
| Job slots used | 101 (1 parent + 100 child) | **5** (1 parent + 4 pool) |
| max_worker_processes needed | ≥100 | **≥4** |
| Load balancing | Static (pre-assigned) | **Dynamic** (work-stealing) |

### Limitations

- Maximum 512 tables per pool operation (`PGCLONE_MAX_POOL_TASKS`).
- Only one pool operation can run at a time per database cluster.
- Pool workers are visible in `pgclone_jobs_view` as individual table-type jobs.

---

## Progress Tracking

### Check a specific job

```sql
SELECT pgclone_progress(1);
```

Returns JSON:

```json
{
    "job_id": 1,
    "status": "running",
    "phase": "copying data",
    "tables_completed": 5,
    "tables_total": 12,
    "rows_copied": 450000,
    "current_table": "orders",
    "elapsed_ms": 8500
}
```

### List all jobs

```sql
SELECT pgclone_jobs();
-- Returns JSON array of all active/recent jobs
```

---

## Progress Tracking View (v2.1.0+)

Query live progress of all async clone jobs as a standard PostgreSQL view with visual progress bar and elapsed time:

```sql
SELECT job_id, status, schema_name, progress_bar FROM pgclone_jobs_view;
```

```
 job_id | status    | schema_name | progress_bar
--------+-----------+-------------+------------------------------------------------------------
      1 | running   | sales       | [████████████░░░░░░░░] 60.0% | 450000 rows | 00:08:30 elapsed
      2 | pending   | public      | [░░░░░░░░░░░░░░░░░░░░] 0.0% | 0 rows | 00:00:00 elapsed
      3 | completed | analytics   | [████████████████████] 100.0% | 1200000 rows | 00:25:18 elapsed
```

### Filter by status

```sql
-- Running jobs with elapsed time
SELECT job_id, status, elapsed_time, pct_complete
FROM pgclone_jobs_view
WHERE status = 'running';

-- Failed jobs with error messages
SELECT job_id, schema_name, error_message
FROM pgclone_jobs_view
WHERE status = 'failed';
```

### Full detail

```sql
SELECT * FROM pgclone_jobs_view;

-- Or via the underlying function:
SELECT * FROM pgclone_progress_detail();
```

---

## Job Management

### Cancel a running job

```sql
SELECT pgclone_cancel(1);
```

### Resume a failed job

Resumes from the last checkpoint, returns a new job_id:

```sql
SELECT pgclone_resume(1);
-- Returns: 2
```

### Clear completed/failed jobs

```sql
SELECT pgclone_clear_jobs();
-- Returns: number of jobs cleared
```

---

## Conflict Resolution in Async Mode

All conflict strategies work with async functions:

```sql
-- Skip if table exists
SELECT pgclone_table_async(conn, 'public', 'orders', true, 'orders',
    '{"conflict": "skip"}');

-- Drop and re-create
SELECT pgclone_table_async(conn, 'public', 'orders', true, 'orders',
    '{"conflict": "replace"}');

-- Rename existing table
SELECT pgclone_table_async(conn, 'public', 'orders', true, 'orders',
    '{"conflict": "rename"}');
```

---

## How It Works

1. When you call an async function, pgclone registers a background worker with PostgreSQL's `BackgroundWorker` API.
2. The background worker starts in a separate process, connects to both source and target databases using `libpq`, and performs the clone operation.
3. Progress is tracked in shared memory (`pgclone_state`), which is allocated via `shmem_request_hook` (PG 15+) or `RequestAddinShmemSpace` (PG 14).
4. The `pgclone_jobs_view` reads shared memory to display real-time progress.
5. For parallel cloning (v2.2.0+), the parent process populates a shared-memory task queue and launches exactly N pool workers. Each worker pulls tasks from the queue until it's empty — providing dynamic load balancing with O(N) resource usage.

**Tip:** Verbose per-table/per-row NOTICE messages have been moved to DEBUG1 level. To see them:

```sql
SET client_min_messages = debug1;
```
