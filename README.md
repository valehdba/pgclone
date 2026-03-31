# PgClone

A PostgreSQL extension written in C for cloning databases, schemas, tables, and functions between PostgreSQL hosts — directly from SQL.
 
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
-- Returns: pgclone 2.0.0
```

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

## Current Limitations (v2.0.0)

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
- [ ] ~~v2.0.1: CREATE database if database does not exist during SELECT pgclone_database('source_db', 'target_db').
- [ ] ~~v2.1.0: Applying Static Data Masking to cloned data 
- [ ] ~~v2.1.1: Applying Dynamic Data Masking to cloned data 


## License

PostgreSQL License
