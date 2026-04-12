# PgClone Manual Testing Guide

Step-by-step manual test scenarios for validating all pgclone functionalities on real infrastructure with two PostgreSQL instances.

---

## Environment

Replace connection details with your actual servers.

| Parameter | Server 1 (Source) | Server 2 (Target) |
|-----------|-------------------|-------------------|
| IP Address | `172.17.0.2` | `172.17.0.3` |
| Port | `5432` | `5433` |
| Database | `db1` | `db2` |
| User / Pass | `postgres` / `123654` | `postgres` / `123654` |
| pgclone | Installed + preloaded | Installed + preloaded |
| Auth | `scram-sha-256` | `scram-sha-256` |

Connection string used throughout:

```
host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654
```

All SQL commands run on **Server 2 (Target)** unless stated otherwise.

---

## Pre-Test: Seed Source Database (Server 1)

Connect to Server 1 and load the project seed file:

```bash
psql -h 172.17.0.2 -p 5432 -U postgres -d db1 -f test/fixtures/seed.sql
```

This creates `test_schema` with tables (`customers`, `orders`, `order_items`, `employees`), indexes, constraints, triggers, views, materialized views, functions, sequences, roles (`test_reader`, `test_writer`, `test_admin`), and sample data.

## Pre-Test: Prepare Target (Server 2)

```sql
psql -h 172.17.0.3 -p 5433 -U postgres -d db2

CREATE EXTENSION IF NOT EXISTS pgclone;
SELECT pgclone_version();
-- Expected: pgclone 3.6.0
```

---

## Test 1: Extension Installation & Version

```sql
SELECT pgclone_version();
```

**Expected:** `pgclone 3.6.0`

```sql
SELECT * FROM pg_extension WHERE extname = 'pgclone';
```

**Expected:** One row with `extname = 'pgclone'`

---

## Test 2: Synchronous Table Cloning

### 2.1 Clone table with data

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true
);
```

**Expected:** `OK: cloned test_schema.customers (10 rows)`

```sql
SELECT COUNT(*) FROM test_schema.customers;
```

**Expected:** 10

### 2.2 Clone structure only (no data)

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'orders', false
);
```

```sql
SELECT COUNT(*) FROM test_schema.orders;
```

**Expected:** 0 (table exists but empty)

### 2.3 Clone with different target name

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true,
    'customers_backup'
);
```

```sql
SELECT COUNT(*) FROM test_schema.customers_backup;
```

**Expected:** 10

### 2.4 Selective column cloning

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true,
    'customers_lite',
    '{"columns": ["id", "name", "email"]}'
);
```

```sql
SELECT column_name FROM information_schema.columns
WHERE table_schema = 'test_schema' AND table_name = 'customers_lite'
ORDER BY ordinal_position;
```

**Expected:** Only 3 columns: `id`, `name`, `email`

### 2.5 Clone with WHERE filter

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true,
    'active_customers_copy',
    '{"where": "status = ''active''"}'
);
```

```sql
SELECT COUNT(*) FROM test_schema.active_customers_copy;
SELECT DISTINCT status FROM test_schema.active_customers_copy;
```

**Expected:** 7 rows, all with `status = 'active'`

### 2.6 Columns + WHERE combined

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true,
    'high_scorers',
    '{"columns": ["id", "name", "score"], "where": "score >= 80"}'
);
```

```sql
SELECT * FROM test_schema.high_scorers ORDER BY score DESC;
```

**Expected:** Only rows with `score >= 80`, only 3 columns

---

## Test 3: Indexes, Constraints & Triggers Control

### 3.1 Clone without indexes

```sql
DROP TABLE IF EXISTS test_schema.customers CASCADE;

SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true,
    'customers',
    '{"indexes": false}'
);
```

```sql
SELECT indexname FROM pg_indexes
WHERE schemaname = 'test_schema' AND tablename = 'customers';
```

**Expected:** Only PK index (`customers_pkey`). No `idx_customers_status` or `idx_customers_name_lower`.

### 3.2 Clone without constraints

```sql
DROP TABLE IF EXISTS test_schema.order_items CASCADE;
DROP TABLE IF EXISTS test_schema.orders CASCADE;

SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'orders', true,
    'orders',
    '{"constraints": false}'
);
```

```sql
SELECT conname, contype FROM pg_constraint
WHERE conrelid = 'test_schema.orders'::regclass;
```

**Expected:** No FK or CHECK constraints (only PK remains)

### 3.3 Clone without triggers

```sql
DROP TABLE IF EXISTS test_schema.orders CASCADE;

SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'orders', true,
    'orders',
    '{"triggers": false}'
);
```

```sql
SELECT tgname FROM pg_trigger
WHERE tgrelid = 'test_schema.orders'::regclass AND NOT tgisinternal;
```

**Expected:** No triggers

### 3.4 pgclone_table_ex() with boolean parameters

```sql
DROP TABLE IF EXISTS test_schema.orders CASCADE;

SELECT pgclone_table_ex(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'orders', true, 'orders',
    false,  -- skip indexes
    true,   -- include constraints
    false   -- skip triggers
);
```

```sql
SELECT conname FROM pg_constraint WHERE conrelid = 'test_schema.orders'::regclass;
SELECT indexname FROM pg_indexes WHERE schemaname = 'test_schema' AND tablename = 'orders';
SELECT tgname FROM pg_trigger WHERE tgrelid = 'test_schema.orders'::regclass AND NOT tgisinternal;
```

**Expected:** Constraints present (PK + FK). Only PK index. No triggers.

---

## Test 4: Conflict Resolution Strategies

### 4.1 Default (error)

```sql
-- Table already exists from previous tests
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true
);
```

**Expected:** ERROR — table already exists

### 4.2 Skip

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true,
    'customers',
    '{"conflict": "skip"}'
);
```

**Expected:** Message indicating table was skipped

### 4.3 Replace

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true,
    'customers',
    '{"conflict": "replace"}'
);
```

```sql
SELECT COUNT(*) FROM test_schema.customers;
```

**Expected:** 10 (fresh clone after drop + re-create)

### 4.4 Rename

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true,
    'customers',
    '{"conflict": "rename"}'
);
```

```sql
SELECT tablename FROM pg_tables
WHERE schemaname = 'test_schema' AND tablename LIKE 'customers%'
ORDER BY tablename;
```

**Expected:** Both `customers` (new) and `customers_old` (renamed original)

---

## Test 5: Schema Cloning

### 5.1 Full schema clone with data

```sql
DROP SCHEMA IF EXISTS test_schema CASCADE;

SELECT pgclone_schema(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', true
);
```

Verify all object types:

```sql
-- Tables
SELECT tablename FROM pg_tables WHERE schemaname = 'test_schema' ORDER BY tablename;

-- Row counts
SELECT 'customers' AS tbl, COUNT(*) FROM test_schema.customers
UNION ALL SELECT 'orders', COUNT(*) FROM test_schema.orders
UNION ALL SELECT 'order_items', COUNT(*) FROM test_schema.order_items
UNION ALL SELECT 'employees', COUNT(*) FROM test_schema.employees;

-- Views
SELECT viewname FROM pg_views WHERE schemaname = 'test_schema';

-- Materialized views
SELECT matviewname FROM pg_matviews WHERE schemaname = 'test_schema';

-- Functions
SELECT routine_name FROM information_schema.routines
WHERE routine_schema = 'test_schema' ORDER BY routine_name;

-- Sequences
SELECT sequencename FROM pg_sequences WHERE schemaname = 'test_schema';

-- Triggers
SELECT tgname FROM pg_trigger t
JOIN pg_class c ON t.tgrelid = c.oid
JOIN pg_namespace n ON c.relnamespace = n.oid
WHERE n.nspname = 'test_schema' AND NOT t.tgisinternal;

-- Indexes
SELECT indexname FROM pg_indexes WHERE schemaname = 'test_schema' ORDER BY indexname;
```

**Expected:** All tables, views, materialized views, functions, sequences, triggers, and indexes present with correct data.

### 5.2 Schema clone structure only

```sql
DROP SCHEMA IF EXISTS test_schema CASCADE;

SELECT pgclone_schema(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', false
);
```

```sql
SELECT 'customers' AS tbl, COUNT(*) FROM test_schema.customers
UNION ALL SELECT 'orders', COUNT(*) FROM test_schema.orders;
```

**Expected:** All counts = 0

### 5.3 Schema clone with JSON options

```sql
DROP SCHEMA IF EXISTS test_schema CASCADE;

SELECT pgclone_schema(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', true,
    '{"triggers": false, "indexes": false}'
);
```

```sql
SELECT COUNT(*) FROM pg_trigger t
JOIN pg_class c ON t.tgrelid = c.oid
JOIN pg_namespace n ON c.relnamespace = n.oid
WHERE n.nspname = 'test_schema' AND NOT t.tgisinternal;
```

**Expected:** Trigger count = 0. Only PK indexes present.

### 5.4 pgclone_schema_ex() with boolean parameters

```sql
DROP SCHEMA IF EXISTS test_schema CASCADE;

SELECT pgclone_schema_ex(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', true,
    true,   -- include indexes
    false,  -- skip constraints
    true    -- include triggers
);
```

**Expected:** Indexes and triggers present. No FK/CHECK constraints (except PK).

---

## Test 6: Function Cloning

```sql
DROP SCHEMA IF EXISTS test_schema CASCADE;
CREATE SCHEMA test_schema;

SELECT pgclone_functions(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema'
);
```

```sql
SELECT routine_name, routine_type FROM information_schema.routines
WHERE routine_schema = 'test_schema' ORDER BY routine_name;
```

**Expected:** Functions: `get_customer_orders`, `log_order_change`, `update_timestamp`

---

## Test 7: Database Cloning

### 7.1 Clone into current database

```sql
DROP SCHEMA IF EXISTS test_schema CASCADE;
DROP TABLE IF EXISTS public.simple_test CASCADE;

SELECT pgclone_database(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    true
);
```

```sql
SELECT schemaname, COUNT(*) AS table_count
FROM pg_tables
WHERE schemaname NOT IN ('pg_catalog', 'information_schema')
GROUP BY schemaname ORDER BY schemaname;
```

**Expected:** Both `public` and `test_schema` tables cloned with data.

### 7.2 Clone database with options

```sql
DROP SCHEMA IF EXISTS test_schema CASCADE;
DROP TABLE IF EXISTS public.simple_test CASCADE;

SELECT pgclone_database(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    true,
    '{"triggers": false}'
);
```

**Expected:** All data cloned, no triggers on any table.

### 7.3 Clone into a new database (pgclone_database_create)

> **Run from the `postgres` database on Server 2, not `db2`.**

```sql
psql -h 172.17.0.3 -p 5433 -U postgres -d postgres

CREATE EXTENSION IF NOT EXISTS pgclone;

SELECT pgclone_database_create(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'db1_clone',
    true
);
```

```sql
\c db1_clone

SELECT schemaname, COUNT(*) AS table_count
FROM pg_tables
WHERE schemaname NOT IN ('pg_catalog', 'information_schema')
GROUP BY schemaname;
```

**Expected:** New database `db1_clone` created with all schemas and data from source.

---

## Test 8: Data Masking During Clone (v3.0.0)

Masking is applied server-side during COPY — source data never stored unmasked on target.

### 8.1 Email masking

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true,
    'emp_email_masked',
    '{"mask": {"email": "email"}}'
);

SELECT id, full_name, email FROM test_schema.emp_email_masked ORDER BY id;
```

**Expected:** `alice@example.com` → `a***@example.com`

### 8.2 Name masking

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true,
    'emp_name_masked',
    '{"mask": {"full_name": "name"}}'
);

SELECT id, full_name FROM test_schema.emp_name_masked ORDER BY id;
```

**Expected:** All `full_name` = `XXXX`

### 8.3 Phone masking

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true,
    'emp_phone_masked',
    '{"mask": {"phone": "phone"}}'
);

SELECT id, phone FROM test_schema.emp_phone_masked ORDER BY id;
```

**Expected:** `+1-555-123-4567` → `***-4567`. Eve's NULL phone stays NULL.

### 8.4 Hash masking (MD5)

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true,
    'emp_hash_masked',
    '{"mask": {"email": "hash"}}'
);

SELECT id, email FROM test_schema.emp_hash_masked ORDER BY id;
```

**Expected:** Emails are 32-character hex MD5 hashes. Same input → same hash (deterministic).

### 8.5 Null masking

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true,
    'emp_null_masked',
    '{"mask": {"ssn": "null"}}'
);

SELECT id, ssn FROM test_schema.emp_null_masked ORDER BY id;
```

**Expected:** All SSN values are NULL.

### 8.6 Partial masking

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true,
    'emp_partial_masked',
    '{"mask": {"full_name": {"type": "partial", "prefix": 2, "suffix": 3}}}'
);

SELECT id, full_name FROM test_schema.emp_partial_masked ORDER BY id;
```

**Expected:** `Alice Johnson` → `Al***son`

### 8.7 Random integer masking

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true,
    'emp_rand_masked',
    '{"mask": {"salary": {"type": "random_int", "min": 30000, "max": 150000}}}'
);

SELECT id, salary FROM test_schema.emp_rand_masked ORDER BY id;
```

**Expected:** Salaries are random integers between 30000 and 150000.

### 8.8 Constant masking

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true,
    'emp_const_masked',
    '{"mask": {"notes": {"type": "constant", "value": "REDACTED"}}}'
);

SELECT id, notes FROM test_schema.emp_const_masked ORDER BY id;
```

**Expected:** All notes = `REDACTED` (including previously NULL values).

### 8.9 Multiple masks combined

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true,
    'emp_full_masked',
    '{"mask": {"email": "email", "full_name": "name", "phone": "phone", "ssn": "null", "salary": {"type": "random_int", "min": 40000, "max": 200000}, "notes": {"type": "constant", "value": "REDACTED"}}}'
);

SELECT * FROM test_schema.emp_full_masked ORDER BY id;
```

**Expected:** All sensitive columns masked. `id` and `created_at` pass through unchanged.

---

## Test 9: Auto-Discovery of Sensitive Data (v3.1.0)

```sql
SELECT pgclone_discover_sensitive(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema'
);
```

**Expected:** JSON output with detected sensitive columns grouped by table, e.g.:

```json
{"employees": {"email": "email", "full_name": "name", "phone": "phone", "salary": "random_int", "ssn": "null"}}
```

The output can be used directly as the `"mask"` option in a clone call.

---

## Test 10: Static Data Masking (v3.2.0)

Apply masking to an already-existing local table.

### 10.1 Clone without masking first

```sql
DROP TABLE IF EXISTS test_schema.emp_for_static_mask;

SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true,
    'emp_for_static_mask'
);

-- Verify unmasked data
SELECT full_name, email, ssn FROM test_schema.emp_for_static_mask ORDER BY id;
```

**Expected:** Original data visible.

### 10.2 Apply in-place masking

```sql
SELECT pgclone_mask_in_place(
    'test_schema', 'emp_for_static_mask',
    '{"email": "email", "full_name": "name", "ssn": "null"}'
);
```

**Expected:** `OK: masked 5 rows in test_schema.emp_for_static_mask (3 columns)`

```sql
SELECT full_name, email, ssn FROM test_schema.emp_for_static_mask ORDER BY id;
```

**Expected:** `full_name = XXXX`, `email` masked, `ssn = NULL`

---

## Test 11: Dynamic Data Masking (v3.3.0)

Role-based masking policies that preserve original data.

### 11.1 Prepare

```sql
DROP TABLE IF EXISTS test_schema.employees CASCADE;

SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'data_admin') THEN
        CREATE ROLE data_admin WITH LOGIN PASSWORD 'admin123';
    END IF;
END $$;
```

### 11.2 Create masking policy

```sql
SELECT pgclone_create_masking_policy(
    'test_schema', 'employees',
    '{"email": "email", "full_name": "name", "ssn": "null", "salary": {"type": "random_int", "min": 40000, "max": 200000}}',
    'data_admin'
);
```

**Expected:** OK. Creates `test_schema.employees_masked` view.

### 11.3 Verify masked view

```sql
SELECT * FROM test_schema.employees_masked ORDER BY id;
```

**Expected:** Masked data — `XXXX` names, masked emails, NULL ssn, random salaries.

### 11.4 Verify original data intact

```sql
SELECT full_name, email, ssn, salary FROM test_schema.employees ORDER BY id;
```

**Expected:** Original data unchanged (Alice Johnson, alice@example.com, 123-45-6789, 95000).

### 11.5 Test role-based access

```sql
SET ROLE data_admin;
SELECT full_name, email, ssn FROM test_schema.employees ORDER BY id;
RESET ROLE;
```

**Expected:** `data_admin` can see original unmasked data via the base table.

### 11.6 Drop masking policy

```sql
SELECT pgclone_drop_masking_policy('test_schema', 'employees');

SELECT viewname FROM pg_views
WHERE schemaname = 'test_schema' AND viewname = 'employees_masked';
```

**Expected:** No rows — view dropped, base table access restored to PUBLIC.

---

## Test 12: Clone Roles & Permissions (v3.4.0)

### 12.1 Clone all roles

```sql
SELECT pgclone_clone_roles(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654'
);
```

**Expected:** `OK: N roles created, N roles updated, N grants applied`

```sql
SELECT rolname, rolcanlogin, rolcreatedb FROM pg_roles
WHERE rolname IN ('test_reader', 'test_writer', 'test_admin')
ORDER BY rolname;
```

**Expected:** All three roles exist with correct attributes.

### 12.2 Clone specific roles

```sql
DROP ROLE IF EXISTS test_reader;
DROP ROLE IF EXISTS test_writer;

SELECT pgclone_clone_roles(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_reader, test_writer'
);
```

```sql
SELECT rolname FROM pg_roles WHERE rolname IN ('test_reader', 'test_writer') ORDER BY rolname;
```

**Expected:** `test_reader` and `test_writer` exist.

### 12.3 Verify permissions

```sql
SELECT grantee, table_schema, table_name, privilege_type
FROM information_schema.table_privileges
WHERE grantee = 'test_reader' AND table_schema = 'test_schema'
ORDER BY table_name, privilege_type;
```

**Expected:** `test_reader` has SELECT on all `test_schema` tables.

### 12.4 Existing role update

```sql
SELECT pgclone_clone_roles(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_admin'
);
```

**Expected:** Returns with `N roles updated`. Attributes and password synced.

---

## Test 13: Clone Verification (v3.5.0)

### 13.1 Prepare

```sql
DROP SCHEMA IF EXISTS test_schema CASCADE;

SELECT pgclone_schema(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', true
);
```

### 13.2 Verify specific schema

```sql
SELECT * FROM pgclone_verify(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema'
);
```

**Expected:** All tables show ✓ (matching row counts).

### 13.3 Verify all schemas

```sql
SELECT * FROM pgclone_verify(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654'
);
```

**Expected:** Rows for all tables across all user schemas.

### 13.4 Verify with intentional mismatch

```sql
DELETE FROM test_schema.customers WHERE id > 5;
ANALYZE test_schema.customers;

SELECT * FROM pgclone_verify(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema'
);
```

**Expected:** `customers` shows ✗ — `source_rows=10`, `target_rows=5`.

---

## Test 14: GDPR/Compliance Masking Report (v3.6.0)

### 14.1 Report on unmasked schema

```sql
SELECT * FROM pgclone_masking_report('test_schema');
```

**Expected:** Lists sensitive columns with sensitivity categories (`Email`, `PII - Name`, `Phone`, `Financial`, `National ID`), `mask_status = UNMASKED`, and recommended strategies.

### 14.2 Apply policy then re-check

```sql
SELECT pgclone_create_masking_policy(
    'test_schema', 'employees',
    '{"email": "email", "full_name": "name", "ssn": "null"}',
    'data_admin'
);

SELECT * FROM pgclone_masking_report('test_schema');
```

**Expected:** Employee columns now show `mask_status = MASKED (view)`.

```sql
-- Cleanup
SELECT pgclone_drop_masking_policy('test_schema', 'employees');
```

---

## Test 15: Async Operations & Background Workers

> Requires `shared_preload_libraries = 'pgclone'` (already configured).

### 15.1 Async table clone

```sql
DROP TABLE IF EXISTS test_schema.customers CASCADE;

SELECT pgclone_table_async(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true
);
```

**Expected:** Returns a job_id (integer).

### 15.2 Check progress

```sql
SELECT pgclone_progress(1);

SELECT * FROM pgclone_jobs_view;
```

**Expected:** Shows status (`pending`/`running`/`completed`), `rows_copied`, `progress_bar`, `elapsed_time`.

### 15.3 Async schema clone

```sql
DROP SCHEMA IF EXISTS test_schema CASCADE;

SELECT pgclone_schema_async(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', true
);

SELECT job_id, status, schema_name, progress_bar FROM pgclone_jobs_view;
```

**Expected:** Schema clone progress with table-level tracking.

### 15.4 List all jobs

```sql
SELECT pgclone_jobs();
```

**Expected:** JSON array of all jobs.

### 15.5 Async with conflict strategy

```sql
SELECT pgclone_table_async(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true,
    'customers',
    '{"conflict": "replace"}'
);
```

**Expected:** Job starts, replaces existing table.

### 15.6 Cancel a job

```sql
-- Start a clone and cancel it (use the returned job_id)
SELECT pgclone_schema_async(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', true,
    '{"conflict": "replace"}'
);

SELECT pgclone_cancel(3);  -- use actual job_id

SELECT status FROM pgclone_jobs_view WHERE job_id = 3;
```

**Expected:** `status = 'cancelled'`

### 15.7 Clear completed jobs

```sql
SELECT pgclone_clear_jobs();

SELECT * FROM pgclone_jobs_view;
```

**Expected:** Returns count of cleared jobs. Only running/pending jobs remain.

---

## Test 16: Parallel Cloning (Worker Pool)

### 16.1 Parallel schema clone

```sql
DROP SCHEMA IF EXISTS test_schema CASCADE;

SELECT pgclone_schema_async(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', true,
    '{"parallel": 4}'
);

SELECT job_id, status, op_type, table_name, progress_bar
FROM pgclone_jobs_view ORDER BY job_id;
```

**Expected:** Parent job + up to 4 pool worker jobs visible.

```sql
-- After completion, verify
SELECT * FROM pgclone_verify(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema'
);
```

**Expected:** All tables match source row counts.

### 16.2 Parallel with combined options

```sql
DROP SCHEMA IF EXISTS test_schema CASCADE;

SELECT pgclone_schema_async(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', true,
    '{"parallel": 4, "conflict": "replace", "triggers": false}'
);
```

**Expected:** Parallel clone completes without triggers.

---

## Test 17: Progress Tracking View

```sql
SELECT pgclone_schema_async(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', true,
    '{"conflict": "replace"}'
);

-- Detailed progress function
SELECT * FROM pgclone_progress_detail();

-- Convenience view
SELECT job_id, status, schema_name, table_name,
       pct_complete, progress_bar, elapsed_time
FROM pgclone_jobs_view;

-- Filter by status
SELECT job_id, status, elapsed_time FROM pgclone_jobs_view WHERE status = 'running';
SELECT job_id, error_message FROM pgclone_jobs_view WHERE status = 'failed';
```

**Expected:** All columns visible — `job_id`, `status`, `op_type`, `schema_name`, `table_name`, `current_phase`, `tables_total`, `tables_completed`, `rows_copied`, `bytes_copied`, `elapsed_ms`, `start_time`, `end_time`, `pct_complete`, `progress_bar`, `elapsed_time`.

---

## Test 18: Edge Cases & Error Handling

### 18.1 Invalid connection string

```sql
SELECT pgclone_table(
    'host=192.168.99.99 dbname=nonexistent user=postgres password=wrong',
    'public', 'test', true
);
```

**Expected:** ERROR — connection failure.

### 18.2 Non-existent table

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'this_table_does_not_exist', true
);
```

**Expected:** ERROR — table not found.

### 18.3 Non-existent schema

```sql
SELECT pgclone_schema(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'nonexistent_schema', true
);
```

**Expected:** ERROR — schema not found.

### 18.4 SQL injection in WHERE clause

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true,
    'injection_test',
    '{"where": "1=1; DROP TABLE test_schema.customers; --"}'
);
```

**Expected:** ERROR — DDL/DML keywords or semicolons rejected.

### 18.5 Invalid mask strategy

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'employees', true,
    'emp_bad_mask',
    '{"mask": {"email": "nonexistent_strategy"}}'
);
```

**Expected:** ERROR — unknown mask strategy.

### 18.6 Invalid JSON options

```sql
SELECT pgclone_table(
    'host=172.17.0.2 port=5432 dbname=db1 user=postgres password=123654',
    'test_schema', 'customers', true,
    'json_test',
    '{"invalid json'
);
```

**Expected:** ERROR — invalid JSON.

---

## Cleanup

```sql
-- On Server 2 (target), connected to db2
DROP SCHEMA IF EXISTS test_schema CASCADE;
DROP TABLE IF EXISTS public.simple_test CASCADE;

DROP ROLE IF EXISTS test_reader;
DROP ROLE IF EXISTS test_writer;
DROP ROLE IF EXISTS test_admin;
DROP ROLE IF EXISTS data_admin;

SELECT pgclone_clear_jobs();

-- To drop the cloned database (connect to postgres db first):
-- \c postgres
-- DROP DATABASE IF EXISTS db1_clone;
```
