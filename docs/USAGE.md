# PgClone Usage Guide

Complete reference for all pgclone functions and options.

## Table of Contents

- [Connection String Format](#connection-string-format)
- [Security Notes](#security-notes)
- [Table Cloning](#table-cloning)
- [Schema Cloning](#schema-cloning)
- [Database Cloning](#database-cloning)
- [Controlling Indexes, Constraints, and Triggers](#controlling-indexes-constraints-and-triggers)
- [Selective Column Cloning](#selective-column-cloning-v110)
- [Data Filtering with WHERE](#data-filtering-with-where-v110)
- [Conflict Resolution](#conflict-resolution-v100)
- [Materialized Views](#materialized-views-v120)
- [Exclusion Constraints](#exclusion-constraints-v120)
- [JSON Options Reference](#json-options-reference)
- [Function Reference](#function-reference)
- [Current Limitations](#current-limitations)

## Connection String Format

All pgclone functions accept a `source_conninfo` parameter using standard PostgreSQL connection strings:

```
host=hostname dbname=database user=username password=password port=5432
```

Or URI format:

```
postgresql://username:password@hostname:5432/database
```

## Security Notes

- This extension requires **superuser** privileges to install and use.
- Connection strings may contain passwords — consider using `.pgpass` files or the `PGPASSFILE` environment variable instead.
- The extension connects to remote hosts using `libpq` — ensure network connectivity and firewall rules allow the connection.
- **WHERE clause protection (v2.2.1):** The `"where"` filter option is validated against DDL/DML keywords and semicolons, and runs inside a `READ ONLY` transaction on the source.

---

## Table Cloning

### Clone a table with data

```sql
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres password=secret',
    'public',           -- schema name
    'customers',        -- table name
    true                -- include data (default: true)
);
```

### Clone structure only (no data)

```sql
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres password=secret',
    'public',
    'customers',
    false
);
```

### Clone with a different target name

```sql
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres password=secret',
    'public',
    'customers',        -- source table name
    true,
    'customers_backup'  -- target table name
);
```

---

## Schema Cloning

Clone an entire schema including tables, views, functions, sequences, materialized views, indexes, constraints, and triggers:

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

---

## Database Cloning

### Clone into the current database

Clone all user schemas from a remote database into the current database:

```sql
SELECT pgclone_database(
    'host=source-server dbname=mydb user=postgres password=secret',
    true                -- include data
);
```

### Clone into a new database (v2.0.1)

Create a new local database and clone everything from a remote source. Run this from the `postgres` database:

```sql
SELECT pgclone_database_create(
    'host=source-server dbname=production user=postgres password=secret',
    'staging_db'            -- target database name (created if not exists)
);

-- Structure only
SELECT pgclone_database_create(
    'host=source-server dbname=production user=postgres password=secret',
    'staging_db',
    false                   -- include_data = false
);

-- With options
SELECT pgclone_database_create(
    'host=source-server dbname=production user=postgres password=secret',
    'staging_db',
    true,
    '{"triggers": false}'
);
```

If the target database already exists, it clones into the existing database. The function automatically installs the pgclone extension in the target database.

---

## Controlling Indexes, Constraints, and Triggers

By default, all indexes, constraints (PK, UNIQUE, CHECK, FK, EXCLUDE), and triggers are cloned. You can disable them using JSON options or explicit boolean parameters.

### JSON options format

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

### Boolean parameters format

```sql
-- pgclone_table_ex(conninfo, schema, table, include_data, target_name,
--                  include_indexes, include_constraints, include_triggers)
SELECT pgclone_table_ex(
    'host=source-server dbname=mydb user=postgres',
    'public', 'orders', true, 'orders_copy',
    false,   -- skip indexes
    true,    -- include constraints
    false    -- skip triggers
);

-- pgclone_schema_ex(conninfo, schema, include_data,
--                   include_indexes, include_constraints, include_triggers)
SELECT pgclone_schema_ex(
    'host=source-server dbname=mydb user=postgres',
    'sales', true,
    true,    -- include indexes
    false,   -- skip constraints
    true     -- include triggers
);
```

---

## Selective Column Cloning (v1.1.0)

Clone only specific columns from a table:

```sql
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres',
    'public', 'users', true, 'users_lite',
    '{"columns": ["id", "name", "email"]}'
);
```

Constraints and indexes referencing columns not included in the selection are automatically filtered out.

---

## Data Filtering with WHERE (v1.1.0)

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

---

## Conflict Resolution (v1.0.0)

Control what happens when a target table already exists:

```sql
-- Error if exists (default)
SELECT pgclone_table(..., '{"conflict": "error"}');

-- Skip if exists
SELECT pgclone_table(..., '{"conflict": "skip"}');

-- Drop and re-create
SELECT pgclone_table(..., '{"conflict": "replace"}');

-- Rename existing to tablename_old
SELECT pgclone_table(..., '{"conflict": "rename"}');
```

Conflict strategy can be combined with other options:

```sql
SELECT pgclone_schema_async(conn, 'sales', true,
    '{"conflict": "replace", "indexes": false, "triggers": false}');
```

---

## Materialized Views (v1.2.0)

Materialized views are cloned automatically during schema clone, including their indexes and data. Disable with:

```sql
SELECT pgclone_schema(conn, 'analytics', true,
    '{"matviews": false}');
```

---

## Exclusion Constraints (v1.2.0)

Exclusion constraints are fully supported and cloned automatically alongside PRIMARY KEY, UNIQUE, CHECK, and FOREIGN KEY constraints.

---

## JSON Options Reference

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `indexes` | bool | `true` | Clone indexes |
| `constraints` | bool | `true` | Clone constraints |
| `triggers` | bool | `true` | Clone triggers |
| `matviews` | bool | `true` | Clone materialized views |
| `columns` | array | all | Columns to include |
| `where` | string | none | Row filter condition |
| `conflict` | string | `"error"` | Conflict strategy: error, skip, replace, rename |
| `parallel` | int | 1 | Number of parallel workers (async only) |

---

## Function Reference

| Function | Returns | Description |
|----------|---------|-------------|
| `pgclone_version()` | text | Extension version string |
| `pgclone_table(conninfo, schema, table, include_data)` | text | Clone a single table |
| `pgclone_table(conninfo, schema, table, include_data, target_name, options)` | text | Clone table with options |
| `pgclone_table_ex(conninfo, schema, table, data, target, idx, constr, trig)` | text | Clone table with boolean flags |
| `pgclone_schema(conninfo, schema, include_data)` | text | Clone entire schema |
| `pgclone_schema(conninfo, schema, include_data, options)` | text | Clone schema with options |
| `pgclone_schema_ex(conninfo, schema, data, idx, constr, trig)` | text | Clone schema with boolean flags |
| `pgclone_functions(conninfo, schema)` | text | Clone functions only |
| `pgclone_database(conninfo, include_data)` | text | Clone database into current DB |
| `pgclone_database(conninfo, include_data, options)` | text | Clone database with options |
| `pgclone_database_create(conninfo, target_db)` | text | Create new DB and clone |
| `pgclone_database_create(conninfo, target_db, include_data, options)` | text | Create new DB and clone with options |
| `pgclone_table_async(...)` | int | Async table clone (returns job_id) |
| `pgclone_schema_async(...)` | int | Async schema clone (returns job_id) |
| `pgclone_progress(job_id)` | json | Job progress as JSON |
| `pgclone_jobs()` | json | All jobs as JSON array |
| `pgclone_cancel(job_id)` | bool | Cancel a running job |
| `pgclone_resume(job_id)` | int | Resume failed job (returns new job_id) |
| `pgclone_clear_jobs()` | int | Clear completed/failed jobs |
| `pgclone_progress_detail()` | setof record | All jobs as table-returning function |
| `pgclone_jobs_view` | view | All jobs with progress bar and elapsed time |

## Current Limitations

- Maximum 512 tables per parallel pool operation.
- Only one pool-based parallel operation can run at a time per cluster.
- `WHERE` clause in data filtering is validated against SQL injection patterns and executed inside a read-only transaction on the source. DDL/DML keywords and semicolons are rejected.
