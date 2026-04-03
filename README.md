# PgClone

<p align="center">
  <img src="logo/pgclone_logo.svg" alt="pgclone logo" width="280"/>
</p>

<p align="center">
  <a href="https://github.com/valehdba/pgclone/actions/workflows/ci.yml"><img src="https://github.com/valehdba/pgclone/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <img src="https://img.shields.io/badge/version-2.1.2-00ff88?style=flat-square&labelColor=1a1a2e" alt="Version">
  <img src="https://img.shields.io/badge/PostgreSQL-14%20|%2015%20|%2016%20|%2017%20|%2018-336791?style=flat-square&logo=postgresql&logoColor=white" alt="PostgreSQL 14-18">
  <img src="https://img.shields.io/badge/language-C-555555?style=flat-square&logo=c&logoColor=white" alt="C">
  <img src="https://img.shields.io/badge/license-PostgreSQL-336791?style=flat-square" alt="License">
  <img src="https://img.shields.io/github/stars/valehdba/pgclone?style=flat-square&color=ffbd2e&labelColor=1a1a2e" alt="Stars">
  <img src="https://img.shields.io/github/issues/valehdba/pgclone?style=flat-square&color=ff5f56&labelColor=1a1a2e" alt="Issues">
</p>

## What is PgClone?
One of the most time-consuming operations in our daily workflow — especially when working with large datasets — is cloning a database. Whether it's refreshing a Staging environment, creating a clean database for automated tests in CI/CD pipelines, or giving developers an isolated environment with production-like data, we constantly need a reliable and fast clone operation.
PgClone is a PostgreSQL extension written in C that lets you clone databases,
schemas, tables, and functions between PostgreSQL instances directly from SQL.
No pg_dump, no pg_restore, no shell scripts — just a function call from any
PostgreSQL client.

## Requirements

- PostgreSQL 14 or later (tested on 14, 15, 16, 17, 18)
- PostgreSQL development headers (`postgresql-server-dev-XX`)
- `libpq` development library (`libpq-dev`)
- GCC or compatible C compiler
- `make` and `pg_config` in your PATH

## Installation

### Install build dependencies

#### Debian / Ubuntu

```bash
sudo apt-get install postgresql-server-dev-18 libpq-dev build-essential
```

#### RHEL / CentOS / Rocky / AlmaLinux

```bash
sudo dnf install postgresql18-devel libpq-devel gcc make
```

#### macOS (Homebrew)

```bash
brew install postgresql@18
```

### Build and install

```bash
git clone https://github.com/valehdba/pgclone.git
cd pgclone
make
sudo make install
```

If you have multiple PostgreSQL versions installed, specify which one:

```bash
make PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config
sudo make install PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config
```

### Enable the extension

```sql
-- Connect to your target database
CREATE EXTENSION pgclone;

-- Verify installation
SELECT pgclone_version();
```

## Usage

### Clone a single table (with data)

```sql
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres password=secret',
    'public',           -- schema name
    'customers',        -- table name
    true                -- include data (default: true)
);
```

### Clone a table (structure only, no data)

```sql
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres password=secret',
    'public',
    'customers',
    false
);
```

### Clone a table with a different name on target

```sql
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres password=secret',
    'public',
    'customers',        -- source table name
    true,               -- include data
    'customers_backup'  -- target table name (will be created as customers_backup)
);
```

### Clone an entire schema (tables + views + functions + sequences)

```sql
SELECT pgclone_schema(
    'host=source-server dbname=mydb user=postgres password=secret',
    'sales',            -- schema to clone
    true                -- include table data
);
```

### Clone only functions from a schema

```sql
SELECT pgclone_functions(
    'host=source-server dbname=mydb user=postgres password=secret',
    'utils'             -- schema containing functions
);
```

### Clone an entire database (all user schemas)

```sql
SELECT pgclone_database(
    'host=source-server dbname=mydb user=postgres password=secret',
    true                -- include data
);
```

### Controlling indexes, constraints, and triggers

By default, all indexes, constraints (PK, UNIQUE, CHECK, FK), and triggers are cloned. You can disable them using JSON options or separate boolean parameters.

#### JSON options format

```sql
-- Clone table without indexes and triggers
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres password=secret',
    'public', 'orders', true, 'orders',
    '{"indexes": false, "triggers": false}'
);

-- Clone schema without any constraints
SELECT pgclone_schema(
    'host=source-server dbname=mydb user=postgres password=secret',
    'sales', true,
    '{"constraints": false}'
);

-- Clone database without triggers
SELECT pgclone_database(
    'host=source-server dbname=mydb user=postgres password=secret',
    true,
    '{"triggers": false}'
);
```

#### Boolean parameters format

```sql
-- pgclone_table_ex(conninfo, schema, table, include_data, target_name,
--                     include_indexes, include_constraints, include_triggers)
SELECT pgclone_table_ex(
    'host=source-server dbname=mydb user=postgres',
    'public', 'orders', true, 'orders_copy',
    false,   -- skip indexes
    true,    -- include constraints
    false    -- skip triggers
);

-- pgclone_schema_ex(conninfo, schema, include_data,
--                      include_indexes, include_constraints, include_triggers)
SELECT pgclone_schema_ex(
    'host=source-server dbname=mydb user=postgres',
    'sales', true,
    true,    -- include indexes
    false,   -- skip constraints
    true     -- include triggers
);
```

### Selective column cloning (v1.1.0)

Clone only specific columns from a table:

```sql
-- Clone only id, name, email columns
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres',
    'public', 'users', true, 'users_lite',
    '{"columns": ["id", "name", "email"]}'
);
```

### Data filtering with WHERE clause (v1.1.0)

Clone only rows matching a condition:

```sql
-- Clone only active users
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres',
    'public', 'users', true, 'active_users',
    '{"where": "status = ''active''"}'
);

-- Combine columns + WHERE + disable triggers
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres',
    'public', 'orders', true, 'recent_orders',
    '{"columns": ["id", "customer_id", "total", "created_at"],
      "where": "created_at > ''2024-01-01''",
      "triggers": false}'
);
```

### Check version

```sql
SELECT pgclone_version();
-- Returns: pgclone 2.1.2
```

### Clone into a new database (v2.0.1)

Create a new local database and clone everything from a remote source into it. Run this from the `postgres` database:

```sql
\c postgres
CREATE EXTENSION pgclone;

-- Clone remote database into a new local database
SELECT pgclone_database_create(
    'host=source-server dbname=production user=postgres password=secret',
    'staging_db'            -- target database name (created if not exists)
);

-- Clone without data (structure only)
SELECT pgclone_database_create(
    'host=source-server dbname=production user=postgres password=secret',
    'staging_db',
    false                   -- include_data = false
);

-- Clone with options (e.g., skip triggers)
SELECT pgclone_database_create(
    'host=source-server dbname=production user=postgres password=secret',
    'staging_db',
    true,
    '{"triggers": false}'
);
```

If the target database already exists, it clones into the existing database. The function automatically installs the pgclone extension in the target database.

## Async Clone Operations (v1.0.0)

Async functions run clone operations in background workers, allowing you to continue using your session while cloning proceeds.

**Prerequisite:** Add to `postgresql.conf`:
```
shared_preload_libraries = 'pgclone'
```
Then restart PostgreSQL.

### Async table clone

```sql
-- Returns job_id
SELECT pgclone_table_async(
    'host=source-server dbname=mydb user=postgres',
    'public', 'large_table', true
);
-- Returns: 1
```

### Async schema clone

```sql
SELECT pgclone_schema_async(
    'host=source-server dbname=mydb user=postgres',
    'sales', true
);
```

### Check progress

```sql
SELECT pgclone_progress(1);
-- Returns JSON:
-- {"job_id": 1, "status": "running", "phase": "copying data",
--  "tables_completed": 5, "tables_total": 12,
--  "rows_copied": 450000, "current_table": "orders", "elapsed_ms": 8500}
```

### List all jobs

```sql
SELECT pgclone_jobs();
-- Returns JSON array of all active/recent jobs
```

### Cancel a job

```sql
SELECT pgclone_cancel(1);
```

### Resume a failed job

```sql
-- Resumes from last checkpoint, returns new job_id
SELECT pgclone_resume(1);
-- Returns: 2
```

## Progress Tracking View (v2.1.0) with Progress Bar (v2.1.1) and Elapsed Time (v2.1.2)

Query live progress of all async clone jobs as a standard PostgreSQL view with a visual progress bar and elapsed time:

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

Separate column for elapsed time:

```sql
SELECT job_id, status, elapsed_time, pct_complete
FROM pgclone_jobs_view
WHERE status = 'running';
```

```
 job_id | status  | elapsed_time | pct_complete
--------+---------+--------------+-------------
      1 | running | 00:08:30     |        60.0
```

Full detail view:

```sql
SELECT * FROM pgclone_jobs_view;
```

You can also filter and monitor specific jobs:

```sql
-- Only running jobs
SELECT job_id, schema_name, current_table, progress_bar, elapsed_ms
FROM pgclone_jobs_view
WHERE status = 'running';

-- Failed jobs with error messages
SELECT job_id, schema_name, error_message
FROM pgclone_jobs_view
WHERE status = 'failed';
```

The underlying function `pgclone_progress_detail()` returns the same data as a table-returning function:

```sql
SELECT * FROM pgclone_progress_detail();
```

**Note:** Requires `shared_preload_libraries = 'pgclone'` in `postgresql.conf`.

**Tip:** Verbose per-table/per-row NOTICE messages have been moved to DEBUG1 level. To see them: `SET client_min_messages = debug1;`

## Conflict Resolution (v1.0.0)

Control what happens when a target table already exists:

```sql
-- Error if exists (default)
SELECT pgclone_table_async(conn, 'public', 'orders', true, 'orders',
    '{"conflict": "error"}');

-- Skip if exists
SELECT pgclone_table_async(conn, 'public', 'orders', true, 'orders',
    '{"conflict": "skip"}');

-- Drop and re-create
SELECT pgclone_table_async(conn, 'public', 'orders', true, 'orders',
    '{"conflict": "replace"}');

-- Rename existing to orders_old
SELECT pgclone_table_async(conn, 'public', 'orders', true, 'orders',
    '{"conflict": "rename"}');
```

Conflict strategy can be combined with other options:
```sql
SELECT pgclone_schema_async(conn, 'sales', true,
    '{"conflict": "replace", "indexes": false, "triggers": false}');
```

## Parallel Cloning (v2.0.0)

Clone tables in parallel using multiple background workers:

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

Each table gets its own background worker. Track all workers via `pgclone_jobs()`.

## Materialized Views (v1.2.0)

Materialized views are now cloned automatically during schema clone, including their indexes and data. Disable with:

```sql
SELECT pgclone_schema(conn, 'analytics', true,
    '{"matviews": false}');
```

## Exclusion Constraints (v1.2.0)

Exclusion constraints are now fully supported and cloned automatically alongside PRIMARY KEY, UNIQUE, CHECK, and FOREIGN KEY constraints.

## Connection String Format

The `source_conninfo` parameter uses standard PostgreSQL connection strings:

```
host=hostname dbname=database user=username password=password port=5432
```

Or URI format:

```
postgresql://username:password@hostname:5432/database
```

## Security Notes

- This extension requires **superuser** privileges to install and use
- Connection strings may contain passwords — consider using `.pgpass` files
  or `PGPASSFILE` environment variable instead
- The extension connects to remote hosts using `libpq` — ensure network
  connectivity and firewall rules allow the connection

## Current Limitations (v2.1.2)

- Parallel cloning uses one bgworker per table — very large schemas may hit max_worker_processes limit
- WHERE clause in data filtering is passed directly to SQL — use with trusted input only

## Roadmap

- [x] ~~v0.1.0: Clone tables, schemas, functions, databases~~ (done)
- [x] ~~v0.1.0: Use COPY protocol for fast data transfer~~ (done)
- [x] ~~v0.2.0: Clone indexes, constraints (PK, UNIQUE, CHECK, FK), and triggers~~ (done)
- [x] ~~v0.2.0: Optional control over indexes/constraints/triggers~~ (done)
- [x] ~~v0.3.0: Background worker for async operations with progress tracking~~ (done)
- [x] ~~v1.0.0: Resume support and conflict resolution~~ (done)
- [x] ~~v1.1.0: Selective column cloning and data filtering~~ (done)
- [x] ~~v1.2.0: Clone materialized views and exclusion constraints~~ (done)
- [x] ~~v2.0.0: True multi-worker parallel cloning~~ (done)
- [x] ~~v2.0.1: CREATE database if database does not exist, from postgres DB - SELECT pgclone_database('source_db', 'target_db').~~ (done)
- [x] ~~v2.1.0: Progress Tracking View~~ (done)
- [x] ~~v2.1.1: Progress Bar instead of NOTICE: pclone XXX row transferred~~ (done)
- [x] ~~v2.1.2: Elapsed Time~~ (done)
- [ ] ~~v2.2.0: Worker pool for parallel cloning (fixed pool size instead of one bgworker per table)
- [ ] ~~v2.2.1: Read-only transaction for WHERE clause execution (SQL injection protection)
- [ ] ~~v3.0.0: Data Anonymization / Masking
- [ ] ~~v3.1.0: Auto-Discovery of Sensitive Data
- [ ] ~~v3.2.1: Applying Static Data Masking to cloned data 
- [ ] ~~v3.2.2: Applying Dynamic Data Masking to cloned data
- [ ] ~~v4.0.0: Copy-on-Write (CoW)  mode for local cloning SELECT pgclone_database_cow('source_db', 'target_db');


## License

PostgreSQL License
