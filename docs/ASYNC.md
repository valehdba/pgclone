# PgClone Async Operations

Async functions run clone operations in PostgreSQL background workers, allowing you to continue using your session while cloning proceeds in the background.

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

## Parallel Cloning (v2.0.0)

Clone tables in parallel using multiple background workers. Each table gets its own background worker:

```sql
-- Clone schema with 4 parallel workers
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
5. For parallel cloning, the parent worker spawns child workers — one per table — and monitors their completion.

**Tip:** Verbose per-table/per-row NOTICE messages have been moved to DEBUG1 level. To see them:

```sql
SET client_min_messages = debug1;
```
