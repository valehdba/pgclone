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
- [Data Masking](#data-masking-v300)
- [Auto-Discovery of Sensitive Data](#auto-discovery-of-sensitive-data-v310)
- [Static Data Masking](#static-data-masking-v320)
- [Dynamic Data Masking](#dynamic-data-masking-v330)
- [Clone Roles and Permissions](#clone-roles-and-permissions-v340)
- [Clone Verification](#clone-verification-v350)
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

## Data Masking (v3.0.0)

Clone tables with column-level data anonymization. Masking is applied server-side as SQL expressions during the COPY stream — no row-by-row overhead.

### Simple Mask Types

```sql
-- Mask email addresses: alice@example.com → a***@example.com
SELECT pgclone_table(conn, 'public', 'users', true, 'users_safe',
    '{"mask": {"email": "email"}}');

-- Replace names with XXXX
SELECT pgclone_table(conn, 'public', 'users', true, 'users_safe',
    '{"mask": {"full_name": "name"}}');

-- Keep last 4 digits of phone: +1-555-123-4567 → ***-4567
SELECT pgclone_table(conn, 'public', 'users', true, 'users_safe',
    '{"mask": {"phone": "phone"}}');

-- Deterministic MD5 hash (preserves referential integrity across tables)
SELECT pgclone_table(conn, 'public', 'users', true, 'users_safe',
    '{"mask": {"email": "hash"}}');

-- Replace with NULL
SELECT pgclone_table(conn, 'public', 'users', true, 'users_safe',
    '{"mask": {"ssn": "null"}}');
```

### Parameterized Mask Types

```sql
-- Partial masking: keep first 2 and last 3 chars
-- "Johnson" → "Jo***son"
SELECT pgclone_table(conn, 'public', 'users', true, 'users_safe',
    '{"mask": {"last_name": {"type": "partial", "prefix": 2, "suffix": 3}}}');

-- Random integer in range
SELECT pgclone_table(conn, 'public', 'users', true, 'users_safe',
    '{"mask": {"salary": {"type": "random_int", "min": 30000, "max": 150000}}}');

-- Fixed replacement value
SELECT pgclone_table(conn, 'public', 'users', true, 'users_safe',
    '{"mask": {"notes": {"type": "constant", "value": "REDACTED"}}}');
```

### Multiple Masks + Other Options

Masks compose with `columns`, `where`, and all other options:

```sql
SELECT pgclone_table(conn, 'hr', 'employees', true, 'employees_dev',
    '{"mask": {"email": "email", "full_name": "name", "ssn": "null", "salary": {"type": "random_int", "min": 40000, "max": 200000}}, "where": "status = ''active''"}');
```

### Mask Strategy Reference

| Strategy | Output | NULL handling |
|----------|--------|---------------|
| `email` | `a***@domain.com` | Preserves NULL |
| `name` | `XXXX` | Preserves NULL |
| `phone` | `***-4567` | Preserves NULL |
| `partial` | `Jo***son` (configurable prefix/suffix) | Preserves NULL |
| `hash` | `5d41402abc4b2a76b9719d911017c592` (MD5) | Preserves NULL |
| `null` | `NULL` | Always NULL |
| `random_int` | Random in `[min, max]` | Ignores NULL (always produces value) |
| `constant` | Fixed value | Ignores NULL (always produces value) |

### Notes

- Masking is applied on the **source** side inside `COPY (SELECT ...) TO STDOUT`, so masked data never enters the local database unmasked.
- The `hash` strategy uses PostgreSQL's built-in `md5()` function — no pgcrypto dependency required. Same input always produces the same hash, so you can maintain referential integrity by hashing the same column across multiple tables.
- Columns not listed in the `mask` object pass through unmodified.
- When combined with `columns`, only the listed columns are cloned; masks apply to those that match.

---

## Auto-Discovery of Sensitive Data (v3.1.0)

Automatically scan a source schema for columns that look like sensitive data:

```sql
SELECT pgclone_discover_sensitive(
    'host=source-server dbname=mydb user=postgres',
    'public'
);
```

Returns JSON grouped by table with suggested mask strategies:

```json
{"employees": {"email": "email", "full_name": "name", "phone": "phone", "salary": "random_int", "ssn": "null"}, "users": {"password": "hash", "api_key": "hash"}}
```

The output can be used directly as the `"mask"` option value in a clone call. Detected patterns include: email, name (first/last/full), phone/mobile, SSN/national ID, salary/income, password/token/api_key, address/street, date of birth, credit card, and IP address.

---

## Static Data Masking (v3.2.0)

Apply masking to an already-cloned local table without needing the source connection:

```sql
-- Mask an existing table in place
SELECT pgclone_mask_in_place(
    'public', 'employees',
    '{"email": "email", "full_name": "name", "ssn": "null"}'
);
-- Returns: OK: masked 1000 rows in public.employees (3 columns)
```

The mask JSON uses the same format as clone-time masking. This is useful for:

- Masking tables that were cloned without masking
- Applying different mask strategies after initial clone
- Sanitizing existing development/staging databases

All 8 mask strategies work: `email`, `name`, `phone`, `partial`, `hash`, `null`, `random_int`, `constant`.

---

## Dynamic Data Masking (v3.3.0)

Create role-based masking policies that **preserve original data** while presenting masked views to unprivileged users. Unlike static masking (which modifies data in-place), dynamic masking keeps the base table intact.

### Create a masking policy

```sql
SELECT pgclone_create_masking_policy(
    'public', 'employees',
    '{"email": "email", "full_name": "name", "ssn": "null"}',
    'data_admin'   -- this role can see unmasked data
);
```

This does four things:

1. Creates a view `public.employees_masked` with mask expressions applied
2. Revokes `SELECT` on `public.employees` from `PUBLIC`
3. Grants `SELECT` on `public.employees_masked` to `PUBLIC`
4. Grants `SELECT` on `public.employees` to `data_admin`

After this, regular users query `employees_masked` and see anonymized data. The `data_admin` role can still query `employees` directly to see raw data.

### Drop a masking policy

```sql
SELECT pgclone_drop_masking_policy('public', 'employees');
```

This drops the `employees_masked` view and re-grants `SELECT` on the base table to `PUBLIC`.

### Typical workflow

```sql
-- 1. Clone production data
SELECT pgclone_table(conn, 'public', 'employees', true);

-- 2. Discover sensitive columns
SELECT pgclone_discover_sensitive(conn, 'public');

-- 3. Apply dynamic masking policy
SELECT pgclone_create_masking_policy(
    'public', 'employees',
    '{"email": "email", "full_name": "name", "salary": {"type": "random_int", "min": 40000, "max": 200000}, "ssn": "null"}',
    'dba_team'
);

-- Regular users see masked data:
-- SELECT * FROM public.employees_masked;
--  id | full_name | email             | salary | ssn
-- ----+-----------+-------------------+--------+------
--   1 | XXXX      | a***@example.com  | 87432  | NULL

-- dba_team role sees raw data:
-- SELECT * FROM public.employees;
--  id | full_name     | email             | salary | ssn
-- ----+---------------+-------------------+--------+-------------
--   1 | Alice Johnson | alice@example.com | 95000  | 123-45-6789
```

### Notes

- The masked view name is always `<table_name>_masked`. If a view with that name already exists, it is replaced (`CREATE OR REPLACE VIEW`).
- All 8 mask strategies from clone-time masking work in dynamic masking.
- The view is a standard PostgreSQL view — it can be queried, joined, and used in subqueries like any other view.
- Dropping the masking policy does not affect the base table data.

---

## Clone Roles and Permissions (v3.4.0)

Clone database roles from a source PostgreSQL instance to the local target, including encrypted passwords, role attributes, memberships, and all privilege grants.

### Import all roles

```sql
SELECT pgclone_clone_roles(
    'host=source-server dbname=mydb user=postgres password=secret'
);
-- OK: 8 roles created, 2 roles updated, 45 grants applied
```

### Import specific roles

```sql
SELECT pgclone_clone_roles(
    'host=source-server dbname=mydb user=postgres password=secret',
    'app_user, reporting_user, api_service'
);
-- OK: 3 roles created, 0 roles updated, 18 grants applied
```

### Import a single role

```sql
SELECT pgclone_clone_roles(
    'host=source-server dbname=mydb user=postgres password=secret',
    'app_user'
);
-- OK: 1 roles created, 0 roles updated, 7 grants applied
```

### What gets cloned

| Category | Details |
|----------|---------|
| Role attributes | LOGIN, SUPERUSER, CREATEDB, CREATEROLE, REPLICATION, INHERIT, CONNECTION LIMIT, VALID UNTIL |
| Passwords | Encrypted password copied from `pg_authid` — password is set identically on target |
| Role memberships | `GRANT role TO role` relationships |
| Schema privileges | USAGE, CREATE on schemas |
| Table privileges | SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER |
| Sequence privileges | USAGE, SELECT, UPDATE |
| Function privileges | EXECUTE on functions and procedures |

### Behavior for existing roles

If a role already exists on the target:

- Password is updated to match the source
- Role attributes (LOGIN, CREATEDB, etc.) are updated to match the source
- Permissions are applied additively (existing grants are not revoked)

### Typical workflow

```sql
-- 1. Clone the database structure and data
SELECT pgclone_database(
    'host=prod dbname=myapp user=postgres',
    true
);

-- 2. Clone all roles and their permissions
SELECT pgclone_clone_roles(
    'host=prod dbname=myapp user=postgres'
);
```

### Requirements

- **Superuser on both source and target** — `pg_authid` (which stores encrypted passwords) is only accessible to superusers
- System roles (`pg_*`) and the `postgres` role are excluded from cloning

---

## Clone Verification (v3.5.0)

Compare row counts between source and target databases to verify clone completeness.

### Verify a specific schema

```sql
SELECT * FROM pgclone_verify(
    'host=source-server dbname=prod user=postgres',
    'app_schema'
);
```

```
 schema_name |  table_name  | source_rows | target_rows | match
-------------+--------------+-------------+-------------+--------------
 app_schema  | customers    |       15230 |       15230 | ✓
 app_schema  | orders       |      148920 |      148920 | ✓
 app_schema  | payments     |       98100 |       97855 | ✗
 app_schema  | audit_log    |     1204500 |           0 | ✗ (missing)
```

### Verify all schemas

```sql
SELECT * FROM pgclone_verify(
    'host=source-server dbname=prod user=postgres'
);
```

Returns one row per table across all user schemas.

### Match indicators

| Indicator | Meaning |
|-----------|---------|
| `✓` | Row counts are equal |
| `✗` | Row counts differ |
| `✗ (missing)` | Table exists on source but not on target |

### Notes

- Row counts use `pg_class.reltuples` for fast approximate counts — no full table scans. Run `ANALYZE` on both source and target for accurate results.
- Works with regular and partitioned tables.
- Useful after `pgclone_schema()` or `pgclone_database()` to confirm all data was transferred.

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
| `mask` | object | none | Column masking rules: `{"col": "type"}` or `{"col": {"type":"...", ...}}` |

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
| `pgclone_discover_sensitive(conninfo, schema)` | text | Scan source for sensitive columns, return mask suggestions as JSON |
| `pgclone_mask_in_place(schema, table, mask_json)` | text | Apply masking to existing local table via UPDATE |
| `pgclone_create_masking_policy(schema, table, mask_json, role)` | text | Create dynamic masking view + role-based access |
| `pgclone_drop_masking_policy(schema, table)` | text | Drop masking view + restore base table access |
| `pgclone_clone_roles(conninfo)` | text | Clone all non-system roles with passwords, attributes, memberships, and permissions |
| `pgclone_clone_roles(conninfo, role_names)` | text | Clone specific roles (comma-separated) with passwords, attributes, and permissions |
| `pgclone_verify(conninfo)` | table | Compare row counts for all tables across source and target |
| `pgclone_verify(conninfo, schema)` | table | Compare row counts for tables in a specific schema |
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
- Data masking (`"mask"` option) is currently supported in synchronous clone functions only. Async background workers do not yet pass mask rules.
