-- ============================================================
-- pgclone pgTAP test suite
-- Run with: pg_prove -d target_db test/pgclone_test.sql
-- ============================================================

BEGIN;

SELECT plan(142);

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
    pgclone.version(),
    '^pgclone ',
    'pgclone.version() returns version string'
);
-- ============================================================
-- TEST GROUP 2: Clone single table (structure + data)
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test'),
    'pgclone.table clones simple_test with data'
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
    format('SELECT pgclone.table(%L, %L, %L, true, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'simple_test_copy'),
    'pgclone.table clones with different target name'
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
    format('SELECT pgclone.table(%L, %L, %L, false, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'simple_test_empty'),
    'pgclone.table clones structure only'
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
    format('SELECT pgclone.schema(%L, %L, true)',
        current_setting('app.source_conninfo'),
        'test_schema'),
    'pgclone.schema clones test_schema'
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
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'customers_lite',
        '{"columns": ["id", "name", "email"]}'),
    'pgclone.table with selective columns'
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
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'active_only',
        '{"where": "status = ''active''"}'),
    'pgclone.table with WHERE filter'
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
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'inject_test1',
        '{"where": "1=1; DROP TABLE customers; --"}'),
    '22023',
    'pgclone: WHERE clause must not contain semicolons',
    'semicolon in WHERE clause is rejected'
);

-- Test: DROP keyword in WHERE clause must be rejected
SELECT throws_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'inject_test2',
        '{"where": "1=1 OR DROP TABLE customers"}'),
    '22023',
    'pgclone: WHERE clause contains forbidden keyword: DROP',
    'DROP keyword in WHERE clause is rejected'
);

-- Test: INSERT keyword in WHERE clause must be rejected
SELECT throws_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'inject_test3',
        '{"where": "1=1 OR INSERT INTO customers VALUES(999)"}'),
    '22023',
    'pgclone: WHERE clause contains forbidden keyword: INSERT',
    'INSERT keyword in WHERE clause is rejected'
);

-- Test: valid WHERE with column named 'created_at' must NOT trigger false positive on CREATE
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'customers', 'no_false_positive',
        '{"where": "created_at IS NOT NULL"}'),
    'WHERE with created_at does not false-positive on CREATE keyword'
);

-- ============================================================
-- TEST GROUP 13: Data masking — email mask
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_email',
        '{"mask": {"email": "email"}}'),
    'pgclone.table with email mask'
);

SELECT has_table('test_schema', 'employees_masked_email', 'masked email table created');

SELECT results_eq(
    'SELECT count(*)::integer FROM test_schema.employees_masked_email',
    ARRAY[5],
    'masked email table has 5 rows'
);

-- Verify emails are masked: local part replaced, domain preserved
SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.employees_masked_email
      WHERE email LIKE 'a%@%' AND email NOT LIKE 'alice@%'$$,
    ARRAY[1],
    'alice email is masked (starts with a but not full address)'
);

-- No original email should survive
SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.employees_masked_email
      WHERE email = 'alice@example.com'$$,
    ARRAY[0],
    'original alice email not present in masked table'
);

-- ============================================================
-- TEST GROUP 14: Data masking — name mask
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_name',
        '{"mask": {"full_name": "name"}}'),
    'pgclone.table with name mask'
);

-- All names should be XXXX
SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.employees_masked_name
      WHERE full_name = 'XXXX'$$,
    ARRAY[5],
    'all names masked to XXXX'
);

-- ============================================================
-- TEST GROUP 15: Data masking — null mask
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_null',
        '{"mask": {"ssn": "null"}}'),
    'pgclone.table with null mask on ssn'
);

SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.employees_masked_null
      WHERE ssn IS NULL$$,
    ARRAY[5],
    'all SSNs nullified'
);

-- ============================================================
-- TEST GROUP 16: Data masking — hash mask (deterministic)
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_hash',
        '{"mask": {"email": "hash"}}'),
    'pgclone.table with hash mask on email'
);

-- Hashed emails should be 32-char hex strings (md5)
SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.employees_masked_hash
      WHERE length(email) = 32 AND email ~ '^[a-f0-9]+$'$$,
    ARRAY[5],
    'all emails are md5 hashes (32 hex chars)'
);

-- ============================================================
-- TEST GROUP 17: Data masking — constant mask
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_const',
        '{"mask": {"notes": {"type": "constant", "value": "REDACTED"}}}'),
    'pgclone.table with constant mask on notes'
);

-- Non-null notes should be REDACTED (NULL notes stay NULL is acceptable too)
SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.employees_masked_const
      WHERE notes = 'REDACTED'$$,
    ARRAY[5],
    'all notes replaced with REDACTED'
);

-- ============================================================
-- TEST GROUP 18: Data masking — combined masks + WHERE
-- ============================================================

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_masked_combo',
        '{"mask": {"email": "email", "full_name": "name", "ssn": "null"}, "where": "salary > 60000"}'),
    'pgclone.table with combined masks and WHERE filter'
);

-- WHERE salary > 60000 should give 4 rows (Alice=95k, Bob=82k, Charlie=67k, Diana=120k)
SELECT results_eq(
    'SELECT count(*)::integer FROM test_schema.employees_masked_combo',
    ARRAY[4],
    'combo masked table has 4 rows (salary > 60000)'
);

-- Names should be masked
SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.employees_masked_combo
      WHERE full_name = 'XXXX'$$,
    ARRAY[4],
    'all names in combo table masked to XXXX'
);

-- ============================================================
-- TEST GROUP 19: Auto-discovery of sensitive data
-- ============================================================

-- Discover sensitive columns in test_schema (from source)
SELECT lives_ok(
    format('SELECT pgclone.discover_sensitive(%L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema'),
    'pgclone.discover_sensitive runs without error'
);

-- Result should contain JSON with employees table detected columns
-- The employees table has: full_name, email, phone, salary, ssn
SELECT ok(
    (SELECT pgclone.discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%email%'),
    'discover detects email column'
);

SELECT ok(
    (SELECT pgclone.discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%full_name%'),
    'discover detects full_name column'
);

SELECT ok(
    (SELECT pgclone.discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%phone%'),
    'discover detects phone column'
);

SELECT ok(
    (SELECT pgclone.discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%salary%'),
    'discover detects salary column'
);

SELECT ok(
    (SELECT pgclone.discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%ssn%'),
    'discover detects ssn column'
);

-- ============================================================
-- TEST GROUP 20: Static masking on cloned data (mask_in_place)
-- ============================================================

-- First: clone employees table without masking
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'employees', 'employees_inplace'),
    'clone employees table for in-place masking'
);

-- Verify original data is present before masking
SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.employees_inplace
      WHERE email = 'alice@example.com'$$,
    ARRAY[1],
    'original alice email present before mask_in_place'
);

-- Apply in-place masking
SELECT lives_ok(
    $$SELECT pgclone.mask_in_place(
        'test_schema', 'employees_inplace',
        '{"email": "email", "full_name": "name", "ssn": "null"}')$$,
    'pgclone.mask_in_place runs without error'
);

-- Verify emails are masked
SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.employees_inplace
      WHERE email = 'alice@example.com'$$,
    ARRAY[0],
    'original alice email removed after mask_in_place'
);

-- Verify names are masked
SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.employees_inplace
      WHERE full_name = 'XXXX'$$,
    ARRAY[5],
    'all names masked to XXXX after mask_in_place'
);

-- Verify SSNs are nullified
SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.employees_inplace
      WHERE ssn IS NULL$$,
    ARRAY[5],
    'all SSNs nullified after mask_in_place'
);

-- Row count should be unchanged
SELECT results_eq(
    'SELECT count(*)::integer FROM test_schema.employees_inplace',
    ARRAY[5],
    'row count unchanged after mask_in_place'
);

-- ============================================================
-- TEST GROUP 21: Schema clone dependency ordering (issue #3)
--
-- Regression test: clone the dep_test fixture schema using a
-- conninfo that pins the source connection's search_path to
-- include dep_test. Under that condition pg_get_triggerdef() and
-- pg_get_expr() emit UNQUALIFIED relation references; without the
-- v4.0.1 fix, the trigger and DEFAULT-nextval clones would fail.
--
-- Also verifies that a SQL-language function whose body references
-- a same-schema table is created successfully (was previously
-- created BEFORE the table existed and failed with
-- "relation does not exist" at CREATE FUNCTION time).
-- ============================================================

-- 11 tests below — keep in sync with plan() above.

SELECT lives_ok(
    format($q$SELECT pgclone.schema(%L, %L, true)$q$,
        current_setting('app.source_conninfo')
            || $$ options='-c search_path=dep_test,public'$$,
        'dep_test'),
    'pgclone.schema clones dep_test under app-style search_path'
);

SELECT has_schema('dep_test', 'dep_test schema exists');

SELECT has_table('dep_test', 'inbox_messages',
    'inbox_messages table cloned (forward-referenced by SQL function)');

SELECT has_table('dep_test', 'documents_to_resend',
    'documents_to_resend table cloned with DEFAULT nextval intact');

SELECT has_table('dep_test', 'city_street',
    'city_street table cloned (target of trigger)');

SELECT has_sequence('dep_test', 'documents_to_resend_id_seq',
    'sequence used as column DEFAULT cloned');

SELECT has_function('dep_test', 'inbox_messages_fct_ck', ARRAY['bigint'],
    'SQL-language function with table reference cloned');

SELECT has_function('dep_test', 'city_street_insert_nextval',
    'trigger function cloned');

SELECT trigger_is(
    'dep_test', 'city_street', 'city_street_insert_trigger',
    'dep_test', 'city_street_insert_nextval',
    'trigger wired to its function on cloned city_street');

-- The cloned function body references dep_test.inbox_messages.
-- Calling it exercises the path that previously failed at CREATE
-- FUNCTION time. Seeded row id=1 must exist; id=999 must not.
SELECT results_eq(
    $$SELECT dep_test.inbox_messages_fct_ck(1)$$,
    ARRAY[true],
    'cloned SQL function returns true for seeded row');

-- Row count and DEFAULT pulling from the cloned sequence.
SELECT results_eq(
    'SELECT count(*)::integer FROM dep_test.documents_to_resend',
    ARRAY[1],
    'documents_to_resend data preserved through clone');

-- ============================================================
-- TEST GROUP 25: v4.4.0 — schema clone with table subset filters
-- and per-table in-line masking ("tables"/"exclude_tables"/"masks")
-- ============================================================
-- 10 tests below — keep in sync with plan() above.

SELECT lives_ok(
    format('SELECT pgclone.schema(%L, %L, true, %L)',
        current_setting('app.source_conninfo'),
        'filter_test',
        '{"tables": ["keep_.*"], "exclude_tables": [".*_old"], '
        || '"masks": {"keep_users": {"email": "email", "ssn": "null"}}}'),
    'pgclone.schema with tables/exclude_tables/masks options'
);

SELECT has_table('filter_test', 'keep_users',
    'included table keep_users cloned');

SELECT has_table('filter_test', 'keep_orders',
    'included table keep_orders cloned');

SELECT hasnt_table('filter_test', 'skip_audit_log',
    'table outside include regex not cloned');

SELECT hasnt_table('filter_test', 'keep_orders_old',
    'table matching exclude regex not cloned');

SELECT results_eq(
    'SELECT count(*)::integer FROM filter_test.keep_users',
    ARRAY[3],
    'masked keep_users has all 3 rows');

-- Email mask: local part replaced, domain preserved
SELECT results_eq(
    $$SELECT count(*)::integer FROM filter_test.keep_users
      WHERE email LIKE 'a%@example.com' AND email <> 'alice@example.com'$$,
    ARRAY[1],
    'keep_users emails masked in-line during schema clone');

SELECT results_eq(
    $$SELECT count(*)::integer FROM filter_test.keep_users WHERE ssn IS NULL$$,
    ARRAY[3],
    'keep_users ssn nulled by masks option');

SELECT results_eq(
    'SELECT count(*)::integer FROM filter_test.keep_orders',
    ARRAY[2],
    'unmasked keep_orders data cloned intact');

-- masks must apply only to the table named in the key
SELECT results_eq(
    $$SELECT count(*)::integer FROM filter_test.keep_orders
      WHERE contact_email IN ('orders1@example.com', 'orders2@example.com')$$,
    ARRAY[2],
    'keep_orders emails untouched — masks scoped per table');

-- ============================================================
-- TEST GROUP 26: issue #17 — type-aware masking
--
-- A mask whose output type the target column cannot store — e.g. the
-- numeric "random_int" strategy on a BOOLEAN column named to match the
-- %income% pattern — used to abort the streaming COPY with
--   ERROR: invalid input syntax for type boolean: "60629"
-- The mask must now be skipped (leaving the column intact) while
-- compatible masks on other columns still apply.
-- ============================================================
-- 8 tests below — keep in sync with plan() above.

-- Clone flags applying random_int to the boolean income_verified column
-- (must be skipped) and email to the string contact_email column (must
-- apply). The clone as a whole must succeed.
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'flags', 'flags_typed',
        '{"mask": {"income_verified": "random_int", "contact_email": "email"}}'),
    'clone with random_int mask on a boolean column succeeds (mask skipped)'
);

SELECT has_table('test_schema', 'flags_typed', 'flags_typed table created');

-- Boolean values pass through unchanged (source has 2 true rows).
SELECT results_eq(
    'SELECT count(*)::integer FROM test_schema.flags_typed WHERE income_verified',
    ARRAY[2],
    'boolean column left intact when its mask is type-incompatible');

-- The compatible email mask still applied — no original address survives.
SELECT results_eq(
    $$SELECT count(*)::integer FROM test_schema.flags_typed
      WHERE contact_email = 'a@example.com'$$,
    ARRAY[0],
    'compatible email mask still applied to string column');

-- discover_sensitive must NOT suggest a mask for the boolean income column.
SELECT ok(
    (SELECT pgclone.discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text NOT LIKE '%income_verified%'),
    'discover skips boolean column income_verified');

-- ...but still suggests one for the numeric income column.
SELECT ok(
    (SELECT pgclone.discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%monthly_income%'),
    'discover still suggests a mask for numeric monthly_income');

-- mask_in_place must skip the incompatible boolean mask and still succeed.
SELECT lives_ok(
    $$SELECT pgclone.mask_in_place(
        'test_schema', 'flags_typed',
        '{"income_verified": "random_int", "monthly_income": "random_int"}')$$,
    'mask_in_place skips incompatible boolean mask and succeeds');

SELECT results_eq(
    'SELECT count(*)::integer FROM test_schema.flags_typed WHERE income_verified',
    ARRAY[2],
    'boolean column unchanged after mask_in_place skips its mask');

-- ============================================================
-- TEST GROUP 27: issue #18 — constraint- and length-aware masking
--
-- A mask must not break the clone by overflowing a column's length,
-- feeding a bad literal to a numeric column, or violating a
-- NOT NULL / UNIQUE / FOREIGN KEY constraint. Such masks are skipped
-- (with a WARNING) and the column is left unmasked; length-limited
-- string columns are clamped so the value always fits.
-- ============================================================
-- 12 tests below — keep in sync with plan() above.

-- Length + numeric-constant: constant on varchar(4) is clamped, constant
-- "REDACTED" on an integer column is skipped — the clone still succeeds.
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'mask18_parent', 'm18_const',
        '{"mask": {"code": {"type":"constant","value":"REDACTED"}, "salary": {"type":"constant","value":"REDACTED"}}}'),
    'constant on varchar(4) + constant on integer: clone succeeds');

SELECT has_table('test_schema', 'm18_const', 'm18_const table created');

SELECT results_eq(
    'SELECT bool_and(length(code) <= 4)::boolean FROM test_schema.m18_const',
    ARRAY[true],
    'constant on varchar(4) clamped to the column length');

SELECT results_eq(
    'SELECT sum(salary)::integer FROM test_schema.m18_const',
    ARRAY[18000],
    'constant on integer skipped — salary values intact');

-- Uniqueness: a collapsing "name" mask on a UNIQUE column is skipped
-- (values stay distinct); "hash" is applied and preserves distinctness.
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'mask18_parent', 'm18_uname', '{"mask": {"email": "name"}}'),
    'name mask on UNIQUE email: clone succeeds (mask skipped)');

SELECT results_eq(
    'SELECT count(DISTINCT email)::integer FROM test_schema.m18_uname',
    ARRAY[3],
    'UNIQUE email left intact when a collapsing mask is requested');

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'mask18_parent', 'm18_uhash', '{"mask": {"email": "hash"}}'),
    'hash mask on UNIQUE email: clone succeeds');

SELECT results_eq(
    $$SELECT count(DISTINCT email)::integer FROM test_schema.m18_uhash
      WHERE email <> 'alice@example.com'$$,
    ARRAY[3],
    'hash mask applied to UNIQUE email and stays distinct');

-- NOT NULL: a null mask on a NOT NULL column is skipped.
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'mask18_parent', 'm18_null', '{"mask": {"ssn": "null"}}'),
    'null mask on NOT NULL ssn: clone succeeds (mask skipped)');

SELECT results_eq(
    'SELECT count(*)::integer FROM test_schema.m18_null WHERE ssn IS NOT NULL',
    ARRAY[3],
    'NOT NULL ssn left intact when a null mask is requested');

-- Foreign key: masking a FK column is skipped (referential integrity).
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema', 'mask18_child', 'm18_child', '{"mask": {"parent_id": {"type":"random_int"}}}'),
    'random_int mask on FK parent_id: clone succeeds (mask skipped)');

-- discover_sensitive steers a UNIQUE sensitive column to hash.
SELECT ok(
    (SELECT pgclone.discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%"email": "hash"%'),
    'discover suggests hash for the UNIQUE email column');

-- ============================================================
-- TEST GROUP 28: issue #22 — conflict resolution in the
-- synchronous clone path (pgclone.table / pgclone.schema)
--
-- "conflict" was parsed but never acted on outside the async
-- bgworker path, so "replace" (and "skip"/"rename"/"error") were
-- silently ignored for sync calls — the target table/sequence just
-- got a plain "CREATE ... IF NOT EXISTS" no matter what strategy
-- was requested.
--
-- NOTE on how "stale" targets are seeded below: pgclone.table()/
-- pgclone.schema() do their local DDL/DML over a brand-new loopback
-- libpq connection (a separate backend session that commits and
-- disconnects on its own), not the pgTAP session running this whole
-- file inside one big BEGIN ... ROLLBACK. Seeding a "stale" target
-- with a plain CREATE TABLE/INSERT here would leave it uncommitted
-- (invisible to that other session) while also holding a lock that
-- session's later DROP/ALTER would wait on — a self-deadlock. So
-- every "stale" target here is created by an *earlier pgclone.table()
-- call* (via its own short-lived committing connection) with a
-- "where" filter that clones a strict subset of the rows; the
-- distinguishing signal is the row count after the real conflict= call
-- — e.g. 6 (1 stale + all 5 appended) if "conflict" were still being
-- silently ignored, vs. 5 (replaced) or 1 (skipped).
-- ============================================================
-- 19 tests below — keep in sync with plan() above.

-- conflict=replace: re-cloning over an existing (1-row) target must
-- drop and re-create it from the source, not append to it.
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'c22_replace', '{"where": "num = 1"}'),
    'c22_replace: seed a stale 1-row target via an earlier plain clone');

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'c22_replace', '{"conflict": "replace"}'),
    'pgclone.table conflict=replace drops and recreates the target');

SELECT results_eq(
    'SELECT count(*)::integer FROM public.c22_replace',
    ARRAY[5],
    'c22_replace holds exactly the source row count — replaced, not appended to');

-- conflict=skip: an existing target table is left completely alone
-- (no re-create, no data copy at all).
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'c22_skip', '{"where": "num = 1"}'),
    'c22_skip: seed a stale 1-row target via an earlier plain clone');

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'c22_skip', '{"conflict": "skip"}'),
    'pgclone.table conflict=skip succeeds without touching the target');

SELECT results_eq(
    'SELECT count(*)::integer FROM public.c22_skip',
    ARRAY[1],
    'c22_skip: untouched — still the one seeded row, nothing appended');

-- conflict=error: an existing target table raises instead of being
-- silently left as plain "CREATE ... IF NOT EXISTS" used to do.
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'c22_error', '{"where": "num = 1"}'),
    'c22_error: seed a stale 1-row target via an earlier plain clone');

SELECT throws_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'c22_error', '{"conflict": "error"}'),
    '42P07',
    'pgclone: table "public"."c22_error" already exists on target',
    'pgclone.table conflict=error raises when the target already exists'
);

-- conflict=rename: the existing target is preserved under a
-- "_old" suffix and a fresh target is cloned in its place.
-- ("constraints": false avoids an unrelated, pre-existing naming
-- collision: a renamed table keeps its PRIMARY KEY's backing index
-- name, e.g. "c22_rename_pkey", which the freshly-created table of
-- the same name would then collide with schema-wide when cloning
-- its own primary key — orthogonal to what issue #22 is about, and
-- not something this test asserts on.)
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'c22_rename',
        '{"where": "num = 1", "constraints": false}'),
    'c22_rename: seed a stale 1-row target via an earlier plain clone');

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'c22_rename',
        '{"conflict": "rename", "constraints": false}'),
    'pgclone.table conflict=rename renames the old target and clones fresh');

SELECT has_table('public', 'c22_rename_old',
    'c22_rename_old exists — the stale target was preserved, not dropped');

SELECT results_eq(
    'SELECT count(*)::integer FROM public.c22_rename',
    ARRAY[5],
    'c22_rename holds the fresh source row count');

SELECT results_eq(
    'SELECT count(*)::integer FROM public.c22_rename_old',
    ARRAY[1],
    'c22_rename_old still holds exactly the stale row');

-- No "conflict" option at all: legacy behavior is unchanged — a
-- plain "CREATE ... IF NOT EXISTS" (existing columns/constraints
-- untouched) and the data copy still runs, appending to what is
-- already there. ("constraints": false on both calls avoids a
-- source-id collision against the PK this table would otherwise
-- inherit from simple_test, which is incidental to what this test
-- is checking.)
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'c22_default',
        '{"where": "num = 1", "constraints": false}'),
    'c22_default: seed a stale 1-row target via an earlier plain clone');

SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'public', 'simple_test', 'c22_default', '{"constraints": false}'),
    'pgclone.table with no "conflict" option keeps the legacy behavior');

SELECT results_eq(
    'SELECT count(*)::integer FROM public.c22_default',
    ARRAY[6],
    'c22_default: legacy behavior unchanged — 1 stale row + all 5 appended');

-- conflict=replace via pgclone.schema: the option must propagate
-- down to each per-table sub-call *and* apply to the sequence
-- created directly by the schema clone (comment on issue #22:
-- "same for sequences"). The stale 1-row target is seeded by a
-- direct pgclone.table() call against the same schema/table (which
-- also pre-creates the dependent sequence), so the real
-- pgclone.schema() call below is the only thing under test.
SELECT lives_ok(
    format('SELECT pgclone.table(%L, %L, %L, true, %L, %L)',
        current_setting('app.source_conninfo'),
        'issue22_schema', 'widget', 'widget', '{"where": "widget_id = 1"}'),
    'issue22_schema.widget: seed a stale 1-row target + its sequence');

SELECT lives_ok(
    format('SELECT pgclone.schema(%L, %L, true, %L)',
        current_setting('app.source_conninfo'),
        'issue22_schema', '{"conflict": "replace"}'),
    'pgclone.schema conflict=replace propagates to the table and its sequence');

SELECT results_eq(
    'SELECT count(*)::integer FROM issue22_schema.widget',
    ARRAY[2],
    'issue22_schema.widget holds exactly the source row count — replaced, not appended to');

-- ============================================================
-- TEST GROUP 29: issue #23 — auto-discover bank account numbers,
-- and audit a remote source with masking_report()
--
-- test_schema.customer_accounts (fixtures/seed.sql) has a NOT NULL
-- bank_account_number and a nullable iban column. NOT NULL steers
-- the rule's default "null" strategy to "hash" (same convention as
-- NOT NULL ssn in mask18_parent); iban keeps "null".
-- ============================================================
-- 7 tests below — keep in sync with plan() above.

SELECT ok(
    (SELECT pgclone.discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%"bank_account_number": "hash"%'),
    'discover steers NOT NULL bank_account_number to hash');

SELECT ok(
    (SELECT pgclone.discover_sensitive(
        current_setting('app.source_conninfo'),
        'test_schema')::text LIKE '%"iban": "null"%'),
    'discover suggests null for the nullable iban column');

SELECT lives_ok(
    format('SELECT * FROM pgclone.masking_report(%L, %L)',
        current_setting('app.source_conninfo'),
        'test_schema'),
    'pgclone.masking_report(conninfo, schema) — remote overload runs without error');

SELECT ok(
    (SELECT EXISTS (
        SELECT 1 FROM pgclone.masking_report(
            current_setting('app.source_conninfo'), 'test_schema')
        WHERE table_name = 'customer_accounts'
          AND column_name = 'bank_account_number'
          AND sensitivity = 'Bank Account'
          AND mask_status = 'UNMASKED'
          AND recommendation LIKE '%hash%'
    )),
    'remote masking_report flags bank_account_number as Bank Account / UNMASKED / hash');

SELECT ok(
    (SELECT EXISTS (
        SELECT 1 FROM pgclone.masking_report(
            current_setting('app.source_conninfo'), 'test_schema')
        WHERE table_name = 'customer_accounts'
          AND column_name = 'iban'
          AND sensitivity = 'Bank Account'
          AND mask_status = 'UNMASKED'
          AND recommendation LIKE '%null%'
    )),
    'remote masking_report flags iban as Bank Account / UNMASKED / null');

-- The original 1-arg local overload still works unmodified — by now
-- test_schema.customer_accounts also exists locally (cloned in TEST
-- GROUP 5's full schema clone, which ran after the fixture was seeded).
SELECT lives_ok(
    format('SELECT * FROM pgclone.masking_report(%L)', 'test_schema'),
    'pgclone.masking_report(schema) — local 1-arg overload still works');

SELECT ok(
    (SELECT EXISTS (
        SELECT 1 FROM pgclone.masking_report('test_schema')
        WHERE table_name = 'customer_accounts'
          AND column_name = 'bank_account_number'
          AND sensitivity = 'Bank Account'
    )),
    'local masking_report also flags the cloned bank_account_number column');

-- ============================================================
-- TEST GROUP 29: Oracle -> PostgreSQL type mapping (no Oracle
-- client needed) and the not-built-with-Oracle error path.
-- 9 tests below — keep in sync with plan() above.
-- ============================================================
SELECT is(pgclone.oracle_type_to_pg('NUMBER', 22, 9, 0, NULL), 'integer',
    'oracle_type_to_pg: NUMBER(9) -> integer');
SELECT is(pgclone.oracle_type_to_pg('NUMBER', 22, 18, 0, NULL), 'bigint',
    'oracle_type_to_pg: NUMBER(18) -> bigint');
SELECT is(pgclone.oracle_type_to_pg('NUMBER', 22, 10, 2, NULL), 'numeric(10,2)',
    'oracle_type_to_pg: NUMBER(10,2) -> numeric(10,2)');
SELECT is(pgclone.oracle_type_to_pg('NUMBER', 22, 3, 5, NULL), 'numeric(5,5)',
    'oracle_type_to_pg: scale > precision widened (PG14-safe)');
SELECT is(pgclone.oracle_type_to_pg('VARCHAR2', 100, NULL, NULL, 25), 'varchar(25)',
    'oracle_type_to_pg: VARCHAR2 uses CHAR_LENGTH');
SELECT is(pgclone.oracle_type_to_pg('DATE', 7, NULL, NULL, NULL), 'timestamp(0)',
    'oracle_type_to_pg: DATE -> timestamp(0)');
SELECT is(pgclone.oracle_type_to_pg('TIMESTAMP(9)', 11, NULL, 9, NULL), 'timestamp(6)',
    'oracle_type_to_pg: TIMESTAMP(9) capped at 6 fractional digits');
SELECT is(pgclone.oracle_type_to_pg('XMLTYPE'), NULL,
    'oracle_type_to_pg: unsupported type -> NULL');
SELECT throws_ok(
    $$SELECT pgclone.table_from_oracle('host=h service=s user=u password=p', 'HR', 'EMP')$$,
    NULL, NULL,
    'table_from_oracle fails cleanly (no Oracle build, or unreachable server)');

SELECT * FROM finish();
ROLLBACK;
