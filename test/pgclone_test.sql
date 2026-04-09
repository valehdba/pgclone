-- ============================================================
-- pgclone pgTAP test suite
-- Run with: pg_prove -d target_db test/pgclone_test.sql
-- ============================================================

BEGIN;

SELECT plan(84);

-- ============================================================
-- TEST GROUP 1: Extension loads correctly
-- ============================================================

SELECT lives_ok(
    'CREATE EXTENSION IF NOT EXISTS pgtap',
    'pgTAP extension loads'
);

SELECT lives_ok(
    'CREATE EXTENSION IF NOT EXISTS pgclone',
    'pgclone extension loads'
);

SELECT matches(
    pgclone_version(),
    '^pgclone ',
    'pgclone_version() returns version string'
);
-- ============================================================
-- TEST GROUP 2: Clone single table (structure + data)
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test'),
    'pgclone_table clones simple_test with data'
);

SELECT has_table('public', 'simple_test', 'simple_test table exists locally');

SELECT results_eq(
    'SELECT count(*)::integer FROM public.simple_test',
    ARRAY[5],
    'simple_test has 5 rows'
);

-- ============================================================
-- TEST GROUP 3: Clone table with different name
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'simple_test_copy'),
    'pgclone_table clones with different target name'
);

SELECT has_table('public', 'simple_test_copy', 'simple_test_copy table exists');

SELECT results_eq(
    'SELECT count(*)::integer FROM public.simple_test_copy',
    ARRAY[5],
    'simple_test_copy has 5 rows'
);

-- ============================================================
-- TEST GROUP 4: Clone table structure only (no data)
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, false, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'simple_test_empty'),
    'pgclone_table clones structure only'
);

SELECT has_table('public', 'simple_test_empty', 'simple_test_empty table exists');

SELECT results_eq(
    'SELECT count(*)::integer FROM public.simple_test_empty',
    ARRAY[0],
    'simple_test_empty has 0 rows (structure only)'
);

-- ============================================================
-- TEST GROUP 5: Clone schema (full)
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_schema(%L, %L, true)',
        current_setting('app.source_conninfo'),
        'test_schema'),
    'pgclone_schema clones test_schema'
);

SELECT has_schema('test_schema', 'test_schema exists');
SELECT has_table('test_schema', 'customers', 'customers table cloned');
SELECT has_table('test_schema', 'orders', 'orders table cloned');
SELECT has_table('test_schema', 'order_items', 'order_items table cloned');

-- Verify data
SELECT results_eq(
    'SELECT count(*)::integer FROM test_schema.customers',
    ARRAY[10],
    'customers has 10 rows'
);

SELECT results_eq(
    'SELECT count(*)::integer FROM test_schema.orders',
    ARRAY[10],
    'orders has 10 rows'
);

-- ============================================================
-- TEST GROUP 6: Indexes cloned
-- ============================================================

SELECT has_index(
    'test_schema', 'customers', 'idx_customers_status',
    'idx_customers_status index cloned'
);

SELECT has_index(
    'test_schema', 'orders', 'idx_orders_customer',
    'idx_orders_customer index cloned'
);

-- ============================================================
-- TEST GROUP 7: Constraints cloned
-- ============================================================

-- Primary keys
SELECT col_is_pk('test_schema', 'customers', 'id', 'customers PK cloned');
SELECT col_is_pk('test_schema', 'orders', 'id', 'orders PK cloned');

-- Foreign keys
SELECT fk_ok(
    'test_schema', 'orders', 'customer_id',
    'test_schema', 'customers', 'id',
    'orders->customers FK cloned'
);

-- ============================================================
-- TEST GROUP 8: Views cloned
-- ============================================================

SELECT has_view('test_schema', 'active_customers', 'active_customers view cloned');
SELECT has_view('test_schema', 'order_summary', 'order_summary view cloned');

-- ============================================================
-- TEST GROUP 9: Functions cloned
-- ============================================================

SELECT has_function(
    'test_schema', 'update_timestamp', 
    'update_timestamp function cloned'
);

SELECT has_function(
    'test_schema', 'get_customer_orders', ARRAY['integer'],
    'get_customer_orders function cloned'
);

-- ============================================================
-- TEST GROUP 10: Selective column cloning
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'customers_lite',
        '{"columns": ["id", "name", "email"]}'),
    'pgclone_table with selective columns'
);

SELECT has_table('test_schema', 'customers_lite', 'customers_lite created');

SELECT results_eq(
    $$SELECT count(*)::integer FROM information_schema.columns
      WHERE table_schema = 'test_schema' AND table_name = 'customers_lite'$$,
    ARRAY[3],
    'customers_lite has exactly 3 columns'
);

-- ============================================================
-- TEST GROUP 11: Data filtering with WHERE
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'active_only',
        '{"where": "status = ''active''"}'),
    'pgclone_table with WHERE filter'
);

SELECT results_eq(
    'SELECT count(*)::integer FROM test_schema.active_only',
    ARRAY[7],
    'active_only has 7 rows (only active customers)'
);

-- ============================================================
-- TEST GROUP 12: WHERE clause SQL injection protection
-- ============================================================

-- Test: semicolon in WHERE clause must be rejected
SELECT throws_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'inject_test1',
        '{\"where\": \"1=1; DROP TABLE customers; --\"}'),
    '22023',
    'pgclone: WHERE clause must not contain semicolons',
    'semicolon in WHERE clause is rejected'
);

-- Test: DROP keyword in WHERE clause must be rejected
SELECT throws_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'inject_test2',
        '{\"where\": \"1=1 OR DROP TABLE customers\"}'),
    '22023',
    'pgclone: WHERE clause contains forbidden keyword: DROP',
    'DROP keyword in WHERE clause is rejected'
);

-- Test: INSERT keyword in WHERE clause must be rejected
SELECT throws_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'inject_test3',
        '{\"where\": \"1=1 OR INSERT INTO customers VALUES(999)\"}'),
    '22023',
    'pgclone: WHERE clause contains forbidden keyword: INSERT',
    'INSERT keyword in WHERE clause is rejected'
);

-- Test: valid WHERE with column named 'created_at' must NOT trigger false positive on CREATE
SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'no_false_positive',
        '{\"where\": \"created_at IS NOT NULL\"}'),
    'WHERE with created_at does not false-positive on CREATE keyword'
);

-- ============================================================
-- TEST GROUP 13: Data masking — email mask
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_email',
        '{"mask": {"email": "email"}}'),
    'pgclone_table with email mask'
);

SELECT has_table('test_schema', 'employees_masked_email', 'masked email table created');

SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_masked_email',
        ARRAY[5],
        'masked email table has 5 rows'
    )$$,
    'masked email table has 5 rows'
);

-- Verify emails are masked: local part replaced, domain preserved
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_masked_email
          WHERE email LIKE ''a%@%'' AND email NOT LIKE ''alice@%''',
        ARRAY[1],
        'alice email is masked (starts with a but not full address)'
    )$$,
    'alice email is masked (starts with a but not full address)'
);

-- No original email should survive
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_masked_email
          WHERE email = ''alice@example.com''',
        ARRAY[0],
        'original alice email not present in masked table'
    )$$,
    'original alice email not present in masked table'
);

-- ============================================================
-- TEST GROUP 14: Data masking — name mask
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_name',
        '{"mask": {"full_name": "name"}}'),
    'pgclone_table with name mask'
);

-- All names should be XXXX
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_masked_name
          WHERE full_name = ''XXXX''',
        ARRAY[5],
        'all names masked to XXXX'
    )$$,
    'all names masked to XXXX'
);

-- ============================================================
-- TEST GROUP 15: Data masking — null mask
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_null',
        '{"mask": {"ssn": "null"}}'),
    'pgclone_table with null mask on ssn'
);

SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_masked_null
          WHERE ssn IS NULL',
        ARRAY[5],
        'all SSNs nullified'
    )$$,
    'all SSNs nullified'
);

-- ============================================================
-- TEST GROUP 16: Data masking — hash mask (deterministic)
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_hash',
        '{"mask": {"email": "hash"}}'),
    'pgclone_table with hash mask on email'
);

-- Hashed emails should be 32-char hex strings (md5)
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_masked_hash
          WHERE length(email) = 32 AND email ~ ''^[a-f0-9]+$''',
        ARRAY[5],
        'all emails are md5 hashes (32 hex chars)'
    )$$,
    'all emails are md5 hashes (32 hex chars)'
);

-- ============================================================
-- TEST GROUP 17: Data masking — constant mask
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_const',
        '{"mask": {"notes": {"type": "constant", "value": "REDACTED"}}}'),
    'pgclone_table with constant mask on notes'
);

-- Non-null notes should be REDACTED (NULL notes stay NULL is acceptable too)
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_masked_const
          WHERE notes = ''REDACTED''',
        ARRAY[5],
        'all notes replaced with REDACTED'
    )$$,
    'all notes replaced with REDACTED'
);

-- ============================================================
-- TEST GROUP 18: Data masking — combined masks + WHERE
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_combo',
        '{"mask": {"email": "email", "full_name": "name", "ssn": "null"}, "where": "salary > 60000"}'),
    'pgclone_table with combined masks and WHERE filter'
);

-- WHERE salary > 60000 should give 4 rows (Alice=95k, Bob=82k, Charlie=67k, Diana=120k)
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_masked_combo',
        ARRAY[4],
        'combo masked table has 4 rows (salary > 60000)'
    )$$,
    'combo masked table has 4 rows (salary > 60000)'
);

-- Names should be masked
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_masked_combo
          WHERE full_name = ''XXXX''',
        ARRAY[4],
        'all names in combo table masked to XXXX'
    )$$,
    'all names in combo table masked to XXXX'
);

-- ============================================================
-- TEST GROUP 19: Auto-discovery of sensitive data
-- ============================================================

-- Discover sensitive columns in test_schema (from source)
SELECT lives_ok(
    format('SELECT pgclone_discover_sensitive(%L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema'),
    'pgclone_discover_sensitive runs without error'
);

-- Result should contain JSON with employees table detected columns
-- The employees table has: full_name, email, phone, salary, ssn
SELECT ok(
    (SELECT pgclone_discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%email%'),
    'discover detects email column'
);

SELECT ok(
    (SELECT pgclone_discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%full_name%'),
    'discover detects full_name column'
);

SELECT ok(
    (SELECT pgclone_discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%phone%'),
    'discover detects phone column'
);

SELECT ok(
    (SELECT pgclone_discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%salary%'),
    'discover detects salary column'
);

SELECT ok(
    (SELECT pgclone_discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%ssn%'),
    'discover detects ssn column'
);

-- ============================================================
-- TEST GROUP 20: Static masking on cloned data (mask_in_place)
-- ============================================================

-- First: clone employees table without masking
SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_inplace'),
    'clone employees table for in-place masking'
);

-- Verify original data is present before masking
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_inplace
          WHERE email = ''alice@example.com''',
        ARRAY[1],
        'original alice email present before mask_in_place'
    )$$,
    'original alice email present before mask_in_place'
);

-- Apply in-place masking
SELECT lives_ok(
    $$SELECT pgclone_mask_in_place(
        'test_schema', 'employees_inplace',
        '{"email": "email", "full_name": "name", "ssn": "null"}')$$,
    'pgclone_mask_in_place runs without error'
);

-- Verify emails are masked
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_inplace
          WHERE email = ''alice@example.com''',
        ARRAY[0],
        'original alice email removed after mask_in_place'
    )$$,
    'original alice email removed after mask_in_place'
);

-- Verify names are masked
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_inplace
          WHERE full_name = ''XXXX''',
        ARRAY[5],
        'all names masked to XXXX after mask_in_place'
    )$$,
    'all names masked to XXXX after mask_in_place'
);

-- Verify SSNs are nullified
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_inplace
          WHERE ssn IS NULL',
        ARRAY[5],
        'all SSNs nullified after mask_in_place'
    )$$,
    'all SSNs nullified after mask_in_place'
);

-- Row count should be unchanged
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_inplace',
        ARRAY[5],
        'row count unchanged after mask_in_place'
    )$$,
    'row count unchanged after mask_in_place'
);

-- ============================================================
-- TEST GROUP 21: Dynamic data masking (create/drop policy)
-- ============================================================

-- First: clone employees for dynamic masking test
SELECT lives_ok(
    format('SELECT pgclone_table(%L, %L, %L, true, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_ddm'),
    'clone employees table for dynamic masking'
);

-- Create a masking policy
SELECT lives_ok(
    $$SELECT pgclone_create_masking_policy(
        'test_schema', 'employees_ddm',
        '{"email": "email", "full_name": "name", "ssn": "null"}',
        'postgres')$$,
    'pgclone_create_masking_policy runs without error'
);

-- Verify the masked view was created
SELECT has_view(
    'test_schema', 'employees_ddm_masked',
    'masked view employees_ddm_masked exists'
);

-- Verify masked view returns masked data
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_ddm_masked
          WHERE full_name = ''XXXX''',
        ARRAY[5],
        'masked view shows XXXX for all names'
    )$$,
    'masked view shows XXXX for all names'
);

SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_ddm_masked
          WHERE ssn IS NULL',
        ARRAY[5],
        'masked view shows NULL for all SSNs'
    )$$,
    'masked view shows NULL for all SSNs'
);

-- Verify row count preserved
SELECT lives_ok(
    $$SELECT results_eq(
        'SELECT count(*)::integer FROM test_schema.employees_ddm_masked',
        ARRAY[5],
        'masked view has 5 rows'
    )$$,
    'masked view has 5 rows'
);

-- Drop the masking policy
SELECT lives_ok(
    $$SELECT pgclone_drop_masking_policy('test_schema', 'employees_ddm')$$,
    'pgclone_drop_masking_policy runs without error'
);

-- ============================================================
-- TEST GROUP 22: Clone roles with permissions
-- ============================================================

-- Clone roles from source
SELECT lives_ok(
    format('SELECT pgclone_clone_roles(%L)',
        current_setting('app.source_conninfo')),
    'pgclone_clone_roles runs without error'
);

-- Verify roles were created
SELECT ok(
    (SELECT EXISTS(SELECT 1 FROM pg_roles WHERE rolname = 'test_reader')),
    'test_reader role exists after clone'
);

SELECT ok(
    (SELECT EXISTS(SELECT 1 FROM pg_roles WHERE rolname = 'test_writer')),
    'test_writer role exists after clone'
);

SELECT ok(
    (SELECT EXISTS(SELECT 1 FROM pg_roles WHERE rolname = 'test_admin')),
    'test_admin role exists after clone'
);

-- Verify role attributes
SELECT ok(
    (SELECT rolcanlogin FROM pg_roles WHERE rolname = 'test_reader'),
    'test_reader has LOGIN attribute'
);

SELECT ok(
    (SELECT rolcreatedb FROM pg_roles WHERE rolname = 'test_admin'),
    'test_admin has CREATEDB attribute'
);

-- ============================================================
-- TEST GROUP 23: Clone verification (pgclone_verify)
-- ============================================================

-- Verify against source for test_schema (already cloned in group 5)
SELECT lives_ok(
    format('SELECT * FROM pgclone_verify(%L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema'),
    'pgclone_verify with schema filter runs without error'
);

-- Verify result has expected columns (5 columns)
SELECT results_eq(
    format($$SELECT count(*)::integer FROM pgclone_verify(%L, %L)$$,
        current_setting('app.source_conninfo'),
        'test_schema'),
    $$SELECT count(*)::integer FROM pg_catalog.pg_class c
      JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
      WHERE n.nspname = 'test_schema' AND c.relkind IN ('r', 'p')$$,
    'verify returns one row per source table in schema'
);

-- Verify all-schemas overload works
SELECT lives_ok(
    format('SELECT * FROM pgclone_verify(%L)',
        current_setting('app.source_conninfo')),
    'pgclone_verify without schema filter runs without error'
);

-- Verify that the customers table shows a match (cloned in group 5)
SELECT results_eq(
    format($$SELECT match FROM pgclone_verify(%L, %L)
             WHERE table_name = 'customers' LIMIT 1$$,
        current_setting('app.source_conninfo'),
        'test_schema'),
    ARRAY['✓'::text],
    'customers table shows match after clone'
);

-- Verify simple_test also matches (cloned in group 2)
SELECT results_eq(
    format($$SELECT match FROM pgclone_verify(%L, %L)
             WHERE table_name = 'simple_test' LIMIT 1$$,
        current_setting('app.source_conninfo'),
        'public'),
    ARRAY['✓'::text],
    'simple_test table shows match after clone'
);

SELECT * FROM finish();
ROLLBACK;
