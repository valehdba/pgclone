-- ============================================================
-- pgx_clone pgTAP test suite
-- Run with: pg_prove -d target_db test/pgx_clone_test.sql
-- ============================================================

BEGIN;

SELECT plan(33);

-- ============================================================
-- TEST GROUP 1: Extension loads correctly
-- ============================================================

SELECT lives_ok(
    'CREATE EXTENSION IF NOT EXISTS pgtap',
    'pgTAP extension loads'
);

SELECT lives_ok(
    'CREATE EXTENSION IF NOT EXISTS pgx_clone',
    'pgx_clone extension loads'
);

SELECT matches(
    pgx_clone_version(),
    '^pgx_clone ',
    'pgx_clone_version() returns version string'
);
-- ============================================================
-- TEST GROUP 2: Clone single table (structure + data)
-- ============================================================

SELECT lives_ok(
    format('SELECT pgx_clone_table(%L, %L, %L, true)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test'),
    'pgx_clone_table clones simple_test with data'
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
    format('SELECT pgx_clone_table(%L, %L, %L, true, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'simple_test_copy'),
    'pgx_clone_table clones with different target name'
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
    format('SELECT pgx_clone_table(%L, %L, %L, false, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'simple_test_empty'),
    'pgx_clone_table clones structure only'
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
    format('SELECT pgx_clone_schema(%L, %L, true)',
        current_setting('app.source_conninfo'),
        'test_schema'),
    'pgx_clone_schema clones test_schema'
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
    format('SELECT pgx_clone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'customers_lite',
        '{"columns": ["id", "name", "email"]}'),
    'pgx_clone_table with selective columns'
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
    format('SELECT pgx_clone_table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'active_only',
        '{"where": "status = ''active''"}'),
    'pgx_clone_table with WHERE filter'
);

SELECT results_eq(
    'SELECT count(*)::integer FROM test_schema.active_only',
    ARRAY[7],
    'active_only has 7 rows (only active customers)'
);

SELECT * FROM finish();
ROLLBACK;
