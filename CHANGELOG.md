# Changelog

All notable changes to pgclone are documented in this file.

## [4.4.1]

### Fixed
- **Type-aware data masking — masks incompatible with a column's type no longer abort the clone** (issue #17). A mask strategy emits a value of a fixed kind — a text string (`email`/`name`/`phone`/`partial`/`hash`), an integer (`random_int`), SQL `NULL` (`null`), or a caller-supplied literal (`constant`). When that value was fed into a column whose type could not parse it, the clone failed deep inside the streaming `COPY`. The reported trigger: `pgclone.discover_sensitive()` matched a **boolean** column `base_income_replacement_allow` against the `%income%` financial pattern and suggested the numeric `random_int` strategy; applying it produced `ERROR: pgclone: COPY completed with error: ERROR: invalid input syntax for type boolean: "60629"`. Every mask-application site now checks the column's `pg_type.typcategory` first and **skips an incompatible mask with a `WARNING`** (leaving the column unchanged) instead of corrupting the `COPY` stream. Text masks fit string columns; `random_int` fits numeric or string columns; `null` and `constant` are left to the server to validate.
- **`pgclone.discover_sensitive()` no longer suggests type-incompatible masks.** The scan now joins `pg_type` and omits a suggestion when the recommended strategy cannot be stored in the column's type (e.g. a numeric strategy for a boolean flag, or a text strategy for a numeric/date column), so a re-generated mask file is safe to apply verbatim.
- **`pgclone.masking_report()`** flags a name-matched but type-incompatible column with a `Review manually: strategy X does not fit <type> column` recommendation rather than advising a mask that would fail.

### Changed
- **`pgclone.mask_in_place()`** skips incompatible column masks (with a `WARNING`) and, when *every* rule is incompatible, returns a clear `OK: masked 0 rows … (0 columns — all mask rules were incompatible with their column types)` instead of failing with an empty `UPDATE … SET`.
- **`pgclone.create_masking_policy()`** leaves an incompatible column unmasked in the generated view, preserving the column's original type instead of silently changing it (e.g. `boolean` → `integer`).

### Added
- Internal C helpers in `src/pgclone.c`: `pgclone_mask_out_kind()` / `pgclone_strategy_fits()` / `pgclone_mask_kind_fits()` (mask-output-kind vs. `typcategory` compatibility), `pgclone_column_typcategory()` (per-column type-category lookup over a libpq connection), and `pgclone_masktype_name()` / `pgclone_typcat_desc()` (human-readable names for `WARNING` messages).
- **pgTAP test group 26** (8 tests, plan 87 → 95) plus a `test_schema.flags` fixture (a boolean `income_verified` column whose name matches `%income%`, alongside a numeric `monthly_income` and string `contact_email`): verifies that a `random_int` mask on the boolean column is skipped and the clone succeeds, that the compatible email mask still applies, that `discover_sensitive` omits the boolean but keeps the numeric column, and that `mask_in_place` skips the incompatible mask.

### Compatibility
- No SQL signature changes — the fix is entirely in the C masking engine. `sql/pgclone--4.4.0--4.4.1.sql` only bumps the installed version.
- Behavior change is limited to masks that previously **crashed** the clone (or, for `create_masking_policy`, silently changed a column's type): those columns are now passed through unmasked with a `WARNING`. Masks that already worked are unaffected.
- Synchronous paths only, as with the rest of the masking feature — `*_async` jobs do not carry JSON options through shared memory and ignore `"mask"`/`"masks"`.
- Pure C-string/libpq implementation; identical behavior on PG 14–18.

## [4.4.0]

### Added
- **Schema/database-level in-line masking — `"masks"` option** (discussion #16). `pgclone.schema()`, `pgclone.database()`, and `pgclone.database_create()` now accept `"masks": {"table": {<mask obj>}, "schema.table": {<mask obj>}}` in the JSON options. Each entry is applied to the matching table *while the data streams* from source to target — the same query-based COPY pipeline as the single-table `"mask"` option — so unmasked data never reaches the target and no post-clone `UPDATE`/`VACUUM` pass or masked-view layer is needed. Keys may be bare table names or schema-qualified; qualified keys win, letting database clones disambiguate identically named tables across schemas. Internally the per-table mask objects are captured as raw JSON during option parsing and re-emitted verbatim as the `"mask"` option of each matching per-table sub-call, so the existing masking pipeline (all 8 strategies) applies unchanged.
- **Table subset filters — `"tables"` / `"exclude_tables"` options** (discussion #16). Both accept an array of POSIX regular expressions, anchored as `^(pattern)$`, evaluated by the *source* server against `pg_tables.tablename` (sent as quoted literals — no regex code in the extension and no injection surface). `"exclude_tables"` is applied after `"tables"`. The filtered table list flows through every downstream phase that iterates tables (FK retry, deferred triggers), while sequences, views, matviews, and functions of the schema are still cloned in full. Character classes like `events_[0-9]{4}` are fully supported — the option parser gained string-aware bracket/brace matching so `]` inside a pattern no longer terminates the array.
- **JSON scanning helpers** in `src/pgclone.c`: `pgclone_json_balanced_end()` (string-aware `{}`/`[]` matching), `pgclone_json_string_end()` (escape-aware string scanning), `pgclone_json_unescape()` (resolves `\"` and `\\`), `pgclone_parse_pattern_array()`, and `pgclone_find_table_mask()`.
- **pgTAP test group 25** (10 tests, plan 77 → 87) plus a new `filter_test` seed schema: verifies include/exclude regex filtering, in-line email/null masking during a schema clone, that masking is scoped to the named table only, and that unfiltered tables keep their data intact.
- `COMMENT ON FUNCTION` documentation for `pgclone.schema(TEXT, TEXT, BOOLEAN, TEXT)` and `pgclone.database(TEXT, BOOLEAN, TEXT)` describing the new options.

### Compatibility
- No SQL signature changes — both features are new keys in the existing JSON options argument. `sql/pgclone--4.3.2--4.4.0.sql` only updates function comments.
- Synchronous paths only: `pgclone.schema_async()` and the parallel worker pool do not carry JSON options through shared memory and ignore `"masks"`/`"tables"`/`"exclude_tables"`, as they already do for `"mask"`.
- Pure C-string/libpq implementation; identical behavior on PG 14–18, no `#if PG_VERSION_NUM` guards needed.

## [4.3.2]

### Fixed
- **Snapshot-keeper resilience on the async path** (issue #9, bgw mirror) — v4.3.1 closed the failure mode on the synchronous code paths in `src/pgclone.c` but left `src/pgclone_bgw.c` untouched. As a result, `pgclone.table_async()`, `pgclone.schema_async()` sequential, and `pgclone.schema_async()` parallel-pool mode could still fail with `ERROR: pgclone: SET TRANSACTION SNAPSHOT … invalid snapshot identifier` on networked source connections when the bgworker keeper transaction was killed by a firewall idle TCP drop or by a non-zero source-side `idle_in_transaction_session_timeout`. This release ports the same four-layer fix to the bgworker translation unit.

### Added
- **`bgw_connect_with_keepalives()`** — mirror of `pgclone_connect_with_keepalives()` for the background-worker path. Parses every source `conninfo` via `PQconninfoParse`, injects `keepalives=1 keepalives_idle=30 keepalives_interval=10 keepalives_count=6` when the user did not specify them, and connects via `PQconnectdbParams`. Both URI (`postgresql://…`) and keyword-form strings are handled. Falls back to plain `PQconnectdb` on parser failure so the caller's existing `PQstatus()`-based error path still surfaces the failure cleanly. Replaces three direct `PQconnectdb()` call sites: `pgclone_bgw_main` (single-job worker), `pgclone_pool_worker_main` (pool worker), and `pgclone_pool_coordinator_main` (pool coordinator).
- **`SET LOCAL idle_in_transaction_session_timeout = 0`** and **`SET LOCAL statement_timeout = 0`** in `bgw_begin_repeatable_read()`. Issued immediately after `BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY`; both GUCs are `PGC_USERSET` and `SET LOCAL` reverts at COMMIT.
- **Clearer diagnostic** in `bgw_begin_with_imported_snapshot()` — when `SET TRANSACTION SNAPSHOT` fails with `invalid snapshot identifier`, the WARNING now includes a HINT pointing at firewall idle drops, `idle_in_transaction_session_timeout`, and the `{"consistent": false}` opt-out, with a reference to issue #9.
- **`bgw_keeper_ping()`** — cheap `SELECT 1` probe on the bgworker-owned snapshot keeper connection. Called every ~5 s (every 50th iteration of the 100 ms `WaitLatch` cycle) inside `pgclone_pool_coordinator_main`'s wait loop. Surfaces a silently-dropped keeper before pool workers waste time waiting on a snapshot the server has already reaped, and sets `pool.snapshot_failed` so every worker aborts cleanly.
- **Regression test `test/test_snapshot_keeper.sh` Group 5** — sets `idle_in_transaction_session_timeout = 1s` on the source role and runs `pgclone.schema_async()` against a 3-table source. Without the v4.3.2 fix the bgworker keeper would be killed mid-clone and the async job would land in `failed`. Skipped automatically when `pgclone` is not in `shared_preload_libraries` (i.e. local Docker runs without async setup).

### Compatibility
- No SQL signature changes. `sql/pgclone--4.3.1--4.3.2.sql` is intentionally empty.
- All four protection layers use libpq/server APIs unchanged across PG 14–18. No `#if PG_VERSION_NUM` guards needed.
- No configuration changes required. Existing async jobs continue to work without modification.

### Internal
- `src/pgclone_bgw.c`: new static helpers `bgw_connect_with_keepalives()` (above the snapshot-helpers block) and `bgw_keeper_ping()` (below `bgw_begin_with_imported_snapshot()`). `bgw_begin_repeatable_read()` extended with the `SET LOCAL` block. The async-path code now exactly mirrors the synchronous-path protections in `src/pgclone.c`, eliminating the known gap disclosed in v4.3.1.

## [4.3.1]

### Fixed
- **Snapshot-keeper resilience under long-running clones** (issue #9) — `pgclone.schema()` and `pgclone.database()` could fail mid-run on remote/networked source connections with the misleading message `ERROR: pgclone: could not import snapshot … invalid snapshot identifier`. Root cause: the snapshot-keeper transaction sat idle-in-transaction for the bulk of the clone, and three independent paths could silently terminate it — firewall/NAT idle TCP drop on the conninfo (no `keepalives=*` were set), a non-zero `idle_in_transaction_session_timeout` on the source server, or `statement_timeout` firing on COMMIT. When the keeper died, PostgreSQL deleted the exported-snapshot file, and every subsequent `SET TRANSACTION SNAPSHOT` importer failed with the misleading message PG emits both for malformed IDs and for IDs whose backing file is gone.

### Added
- **TCP keepalives auto-injected** into every source `conninfo` via `PQconninfoParse` + `PQconnectdbParams`: `keepalives=1 keepalives_idle=30 keepalives_interval=10 keepalives_count=6`. Injected only when the user did **not** already set them; explicit user choices (including `keepalives=0`) are preserved. Both URI (`postgresql://…`) and keyword-form conninfo strings are handled.
- **`SET LOCAL idle_in_transaction_session_timeout = 0`** and **`SET LOCAL statement_timeout = 0`** inside the keeper transaction. Both GUCs are `PGC_USERSET` (no privilege required); `SET LOCAL` reverts at COMMIT so pooled connections never leak settings.
- **`errhint` on snapshot-import failure** — when `SET TRANSACTION SNAPSHOT` fails with `invalid snapshot identifier`, pgclone now attaches a clear hint naming the most likely causes and the `{"consistent": false}` emergency opt-out.
- **`pgclone_keeper_ping()`** — cheap `SELECT 1` on the keeper before each per-table call and before every sub-phase of `pgclone.schema()` (FK retry, views, functions, triggers) and `pgclone.database()` (per-schema loop). Surfaces a clear error naming the *keeper* instead of letting the next importer waste a multi-GB `COPY` before hitting the misleading message.
- **Regression test `test/test_snapshot_keeper.sh`** with 4 groups / 14 assertions:
  - Group 1 — `idle_in_transaction_session_timeout = 1s` on source role (deterministic regression for issue #9)
  - Group 2 — `statement_timeout = 1s` on source role
  - Group 3 — `pgclone.database_create()` keeper across an outer per-schema loop
  - Group 4 — URI vs keyword conninfo form handling + user-supplied keepalive overrides preserved

### Compatibility
- No SQL signature changes. The upgrade script `sql/pgclone--4.3.0--4.3.1.sql` is intentionally empty; `ALTER EXTENSION pgclone UPDATE` simply refreshes metadata.
- All four protection layers use libpq/server APIs that have existed unchanged across PG 14–18. No `#if PG_VERSION_NUM` guards needed.
- No configuration changes required. No data is touched. No backward-incompatible behaviour.

### Known gap (closed in v4.3.2)
- The v4.3.1 fix was applied to the synchronous helpers in `src/pgclone.c` only. The background-worker mirror helpers in `src/pgclone_bgw.c` (`bgw_begin_repeatable_read`, `bgw_export_snapshot`, `bgw_begin_with_imported_snapshot`, and four direct `PQconnectdb()` call sites) did **not** receive the same treatment in this release. Async clones (`pgclone.table_async()`, `pgclone.schema_async()` sequential and parallel-pool) over networked source connections may still hit the same three failure modes on v4.3.1. **Closed in v4.3.2** — see the [4.3.2] entry above for the bgworker mirror fix.

### Internal
- New static helpers `pgclone_connect_with_keepalives()` and `pgclone_keeper_ping()` in `src/pgclone.c`.
- `pgclone_connect()` routes through `pgclone_connect_with_keepalives()` instead of calling `PQconnectdb()` directly. Every source connection therefore receives the keepalive injection, not only the keeper.
- `pgclone_begin_repeatable_read()` issues the `SET LOCAL` timeouts after the `BEGIN`. Because `pgclone_begin_with_imported_snapshot()` and `pgclone_setup_source_txn()` both delegate to it, importers inherit the same protection.
- `docs/ARCHITECTURE.md` adds the "Snapshot-keeper resilience (v4.3.1)" section.

## [4.3.0]

### Added
- **Consistent-snapshot clones** — every clone now reads the source under a `BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY` transaction, so all per-table COPY commands within a single clone observe the same point-in-time view of the source database. This eliminates the foreign-key violations and partial-state anomalies that v4.2.x and earlier could produce when cloning a live OLTP source. Enabled by default for `pgclone.table()`, `pgclone.schema()`, `pgclone.database()`, `pgclone.table_async()`, `pgclone.schema_async()` (sequential and parallel pool modes).
- **Cross-connection snapshot sharing** for multi-connection paths — schema and database clones, and parallel pool mode, use `pg_export_snapshot()` / `SET TRANSACTION SNAPSHOT` so every libpq connection involved in a single clone (per-table sub-calls, FK retry, views, matviews, functions, triggers, and every pool worker) binds to one shared snapshot. Same correctness model as `pg_dump -j`.
- **Snapshot-coordinator background worker** (`pgclone_pool_coordinator_main`) — a dedicated bgworker spawned by `pgclone.schema_async(... '{"parallel": N}')` that opens its own source connection, exports the snapshot, publishes the ID into shared memory for pool workers to import, and sits idle in transaction until every importer has bound, then COMMITs.
- **Opt-out option** — `'{"consistent": false}'` in any options-JSON disables the wrapping for callers who want lower source-side lock pressure or prefer the v4.2.x behaviour.

### Changed
- `pgclone.table()` source connection now runs at REPEATABLE READ READ ONLY for the entire clone (was: no transaction, except for an inner READ ONLY wrap when `WHERE` was set). The old per-table inner `BEGIN TRANSACTION READ ONLY` is now a no-op when an outer snapshot transaction is already open, preserving SQL-injection containment in both call paths.
- `pgclone.schema()` keeps its initial source connection open across all sub-phases as a snapshot keeper. Previously it closed the connection after the table-list read and each sub-phase opened independent connections that could see different points in time.
- `pgclone.database()` follows the same keeper pattern at one level higher; the snapshot ID is propagated to every per-schema sub-call and on through to per-table sub-calls.
- `pgclone.schema_async(... '{"parallel": N}')` now launches `N + 1` background workers (one coordinator plus N pool workers) when in consistent mode. The coordinator job appears in `pgclone.jobs_view` like any other job for full progress visibility.

### Tradeoffs documented
- Long-running clones now hold an open transaction on the source for the entire clone duration, which delays VACUUM cleanup of dead tuples and WAL recycling proportional to clone time. For very long clones on busy sources this may cause table/index bloat and WAL accumulation; opt out with `'{"consistent": false}'` if that is more important than cross-table consistency.
- Hot-standby sources are supported on PostgreSQL ≥ 10 (where `pg_export_snapshot()` works on hot standby).

### Internal
- New `CloneOptions.consistent` (bool) and `CloneOptions.snapshot_id[64]` fields plus parser support for `"consistent": false` and `"snapshot_id": "..."` in options JSON.
- New helpers `pgclone_begin_repeatable_read()`, `pgclone_commit_source()`, `pgclone_export_snapshot()`, `pgclone_begin_with_imported_snapshot()`, `pgclone_setup_source_txn()`, `pgclone_setup_source_txn_done()` in `src/pgclone.c`. Mirror helpers `bgw_begin_repeatable_read()`, `bgw_commit_source()`, `bgw_export_snapshot()`, `bgw_begin_with_imported_snapshot()` in `src/pgclone_bgw.c`.
- New `PgclonePoolQueue` shared-memory fields: `consistent`, `snapshot_ready`, `snapshot_failed`, `snapshot_imported_count`, `snapshot_expected_workers`, `launch_complete`, `snapshot_id[64]`, `coordinator_job_id`.
- New `PgcloneJob.consistent` field so the single-worker async path knows whether to wrap in REPEATABLE READ READ ONLY.

## [4.2.0]

### Added
- **Pre-flight validator (`pgclone.preflight(source_conninfo, schema_name)`)** — read-only sanity check that surfaces issues that would otherwise fail mid-clone. Returns a JSON document with three role-based summary arrays (`errors` / `warnings` / `info`) and a per-check object covering: source/target connection, PostgreSQL versions and major-version compatibility, schema existence on both sides, USAGE/SELECT on source and CREATE on target, estimated source size, current target database size, object counts (tables/views/sequences/indexes), name conflicts on the target schema, extensions installed on source but missing on target, owner/grantee roles missing on target, and non-default tablespaces missing on target. `ready` is `true` only when zero errors are recorded.
- **Loopback test coverage** — new preflight assertions in `test/test_loopback.sh` (function registration, JSON shape, `ready` boolean, fabricated name-conflict surfacing, missing-source-schema error path, STRICT NULL handling, read-only catalog invariant).

### Changed
- **CI runs `test/test_loopback.sh` directly** instead of an inline subset hand-rolled in `.github/workflows/ci.yml`. Going forward, every assertion added to the loopback script is exercised in CI automatically — closing the gap that hid v4.1.0 schema-diff tests from the matrix.

### Internal
- New isolated translation unit `src/pgclone_preflight.c`. Like `src/pgclone_diff.c`, this file does not share helpers with `src/pgclone.c` — the feature is fully additive and trivially auditable.
- Both source and target connections run inside `BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY`. The function never issues DDL or DML on either side.

## [4.1.0]

### Added
- **Schema diff (`pgclone.diff(source_conninfo, schema_name)`)** — read-only DDL drift detection between source and the local target. Returns a JSON document with summary counts plus per-category arrays of `only_in_source` / `only_in_target` / `modified` for tables (with per-column `type` / `not_null` / `default` drift), indexes (excluding those backing constraints), constraints, user-defined triggers, views and materialized views, and sequences. Both source and local connections run inside `BEGIN ... READ ONLY` transactions; the function never executes DDL or DML on either side.

### Internal
- New isolated translation unit `src/pgclone_diff.c`. The diff feature does not share helpers with `src/pgclone.c`, keeping the surface area additive and trivially auditable.
- Catalog queries explicitly use `ORDER BY ... COLLATE "C"` to guarantee identical sort order on both sides regardless of the local lc_collate setting, so the merge-walk comparison is deterministic.

## [4.0.1]

### Fixed
- **Schema clone dependency ordering** (issue #3): `pgclone.schema()` now creates objects in dependency-respecting order — sequences → tables (no triggers) → FK retry → views → matviews → functions → triggers — instead of the previous order which cloned functions before tables. SQL-language functions whose body references a table in the same schema no longer fail with `relation "schema.table" does not exist` at `CREATE FUNCTION` time.
- **Unqualified relation references in extracted DDL** (issue #3): all source libpq connections now `SET search_path = pg_catalog` immediately after connect. This forces `pg_get_triggerdef()`, `pg_get_expr()` (column DEFAULTs), and `pg_get_indexdef()` to emit fully schema-qualified relation names. Previously, source DBs with an application schema on their default `search_path` (a common production pattern) produced DDL like `CREATE TRIGGER ... ON city_street ...` and `DEFAULT nextval('documents_to_resend_id_seq')` that failed when replayed on the target loopback connection. Affects sync paths and both bgworker async paths (sequential and pool).

### Internal
- New `pgclone_normalize_session()` helper in `src/pgclone.c` invoked from `pgclone_connect()`; equivalent inline `SET search_path` calls added at the two `PQconnectdb(job->source_conninfo)` sites in `src/pgclone_bgw.c`.
- `pgclone_schema()` now passes `triggers=false` through to each per-table `pgclone_table()` call and runs a single trigger pass at the end after functions are cloned.

## [4.0.0] — BREAKING

### Changed
- **Schema namespace**: All pgclone functions now live under the `pgclone` schema, created automatically by the extension
  - `pgclone_table(...)` → `pgclone.table(...)`
  - `pgclone_schema(...)` → `pgclone.schema(...)`
  - `pgclone_database(...)` → `pgclone.database(...)`
  - `pgclone_database_create(...)` → `pgclone.database_create(...)`
  - `pgclone_table_async(...)` → `pgclone.table_async(...)`
  - `pgclone_schema_async(...)` → `pgclone.schema_async(...)`
  - `pgclone_progress(...)` → `pgclone.progress(...)`
  - `pgclone_cancel(...)` → `pgclone.cancel(...)`
  - `pgclone_resume(...)` → `pgclone.resume(...)`
  - `pgclone_jobs()` → `pgclone.jobs()`
  - `pgclone_clear_jobs()` → `pgclone.clear_jobs()`
  - `pgclone_progress_detail()` → `pgclone.progress_detail()`
  - `pgclone_jobs_view` → `pgclone.jobs_view`
  - `pgclone_discover_sensitive(...)` → `pgclone.discover_sensitive(...)`
  - `pgclone_mask_in_place(...)` → `pgclone.mask_in_place(...)`
  - `pgclone_create_masking_policy(...)` → `pgclone.create_masking_policy(...)`
  - `pgclone_drop_masking_policy(...)` → `pgclone.drop_masking_policy(...)`
  - `pgclone_clone_roles(...)` → `pgclone.clone_roles(...)`
  - `pgclone_verify(...)` → `pgclone.verify(...)`
  - `pgclone_masking_report(...)` → `pgclone.masking_report(...)`
  - `pgclone_version()` → `pgclone.version()`
  - `pgclone_table_ex(...)` → `pgclone.table_ex(...)`
  - `pgclone_schema_ex(...)` → `pgclone.schema_ex(...)`
  - `pgclone_functions(...)` → `pgclone.functions(...)`
- Extension control file now specifies `schema = pgclone`
- **Upgrade path**: This is a breaking change. Users must `DROP EXTENSION pgclone; CREATE EXTENSION pgclone;` to upgrade from v3.x. All application queries must be updated to use the new `pgclone.` prefix.

## [3.6.0]

### Added
- **GDPR/Compliance Masking Report**: `pgclone_masking_report(schema)` generates an audit report listing all sensitive columns, their masking status, and recommendations
  - Detects sensitive columns using shared `sensitivity_rules` (~40 patterns across 10 categories)
  - Checks if a masked view (`table_masked`) exists for each table
  - `mask_status`: `MASKED (view)` or `UNMASKED`
  - `recommendation`: either "OK - masked via view" or "Apply mask strategy: X"
  - Returns SET OF (schema_name, table_name, column_name, sensitivity, mask_status, recommendation)
  - Useful for GDPR, HIPAA, SOX compliance audits — "prove no PII exists unmasked"

### Changed
- Refactored sensitivity rules into shared `SensitivityRule` struct and `pgclone_match_sensitivity()` helper — used by both `pgclone_discover_sensitive` and `pgclone_masking_report`
- Version bumped to 3.6.0

## [3.5.0]

### Added
- **Clone Verification**: `pgclone_verify()` compares row counts between source and target databases, table by table, returning a side-by-side comparison
  - Two overloads: `pgclone_verify(conninfo)` for all schemas, `pgclone_verify(conninfo, schema)` for a specific schema
  - Returns SET OF (schema_name, table_name, source_rows, target_rows, match)
  - Match indicators: `✓` (equal), `✗` (different), `✗ (missing)` (table not on target)
  - Uses `pg_class.reltuples` for fast approximate counts without full table scans
  - Works with regular and partitioned tables
- 5 new pgTAP tests (84 total): verify function runs, row counts match after clone, correct column count in result

### Changed
- Version bumped to 3.5.0

## [3.4.0]

### Added
- **Clone Roles with Permissions and Passwords**: `pgclone_clone_roles()` clones database roles from source to local, including encrypted passwords, role attributes, and all privilege grants
  - `pgclone_clone_roles(conninfo)` — clone all non-system roles
  - `pgclone_clone_roles(conninfo, 'role1,role2')` — clone specific roles (comma-separated)
  - Clones role attributes: LOGIN, SUPERUSER, CREATEDB, CREATEROLE, REPLICATION, INHERIT, CONNECTION LIMIT, VALID UNTIL
  - Copies encrypted passwords from `pg_authid` (requires superuser on both source and target)
  - If target role already exists: updates password, attributes, and applies permissions additively
  - Clones role memberships (GRANT role TO role)
  - Clones schema-level privileges (USAGE, CREATE) via `aclexplode()`
  - Clones table-level privileges (SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER)
  - Clones sequence privileges (USAGE, SELECT, UPDATE)
  - Clones function/procedure EXECUTE privileges
- `test_reader`, `test_writer`, `test_admin` test fixture roles with graduated permissions
- 6 new pgTAP tests (79 total): role creation, LOGIN attribute, CREATEDB attribute verification

### Changed
- Version bumped to 3.4.0

## [3.3.0]

### Added
- **Dynamic Data Masking via Views**: Role-based masking policies that preserve original data while presenting masked views to unprivileged users
  - `pgclone_create_masking_policy(schema, table, mask_json, privileged_role)`: creates a masked view (`table_masked`), revokes base table SELECT from PUBLIC, grants view access to PUBLIC, grants direct table access to the privileged role
  - `pgclone_drop_masking_policy(schema, table)`: drops the masked view and restores base table access to PUBLIC
  - Uses the same `pgclone_build_mask_expr()` engine as clone-time and static masking — all 8 strategies work
  - Queries local `pg_attribute` for column names, applies mask expressions to matching columns
- 7 new pgTAP tests (73 total): policy creation, view existence, masked data verification, privileged role bypass, policy drop, view cleanup

### Changed
- Version bumped to 3.3.0

## [3.2.0]

### Added
- **Static Data Masking on Local Tables**: `pgclone_mask_in_place(schema, table, mask_json)` applies masking to already-cloned tables via UPDATE statements — no source connection needed
  - Uses the same mask JSON format as clone-time masking: `{"email": "email", "name": "name", "ssn": "null"}`
  - Reuses `pgclone_build_mask_expr()` for consistent masking between clone-time and post-clone paths
  - Wraps user-provided JSON into clone options format for unified parsing
  - Returns summary: `OK: masked N rows in schema.table (M columns)`
- 7 new pgTAP tests (66 total): clone-then-mask workflow, verify original data removed, names/SSNs masked, row count preserved

### Changed
- Version bumped to 3.2.0

## [3.1.0]

### Added
- **Auto-Discovery of Sensitive Data**: `pgclone_discover_sensitive(conninfo, schema_name)` scans the source catalog for columns matching ~40 sensitive data patterns and returns suggested mask rules as JSON
  - Pattern categories: email, name, phone, SSN/national ID, financial (salary/income), secrets/credentials (password/token/api_key), address, date of birth, credit card, IP address
  - Case-insensitive matching with LIKE-style wildcards against column names
  - Output is a JSON object grouped by table, ready to paste into the `"mask"` option: `{"employees": {"email": "email", "salary": "random_int", "ssn": "null"}}`
  - Scans regular tables and partitioned tables (relkind `r` and `p`)
- 6 new pgTAP tests (59 total): discovery function runs, detects email/full_name/phone/salary/ssn columns

### Changed
- Version bumped to 3.1.0

## [3.0.0]

### Added
- **Data Masking / Anonymization**: Column-level data masking applied during cloning via the `"mask"` JSON option
  - 8 masking strategies: `email`, `name`, `phone`, `partial`, `hash`, `null`, `random_int`, `constant`
  - `email`: preserves domain, masks local part (`alice@example.com` → `a***@example.com`)
  - `name`: replaces with `XXXX`
  - `phone`: keeps last 4 digits (`+1-555-123-4567` → `***-4567`)
  - `partial`: configurable prefix/suffix retention (`Johnson` → `Jo***on` with prefix=2, suffix=2)
  - `hash`: deterministic MD5 hash — preserves referential integrity across tables
  - `null`: replaces with NULL
  - `random_int`: random integer in configurable `[min, max]` range
  - `constant`: fixed replacement value
  - All strategies are NULL-safe (NULL inputs produce NULL outputs, except `constant` and `random_int`)
- Masking is applied server-side as SQL expressions inside `COPY (SELECT ...) TO STDOUT` — no row-by-row processing overhead
- Fully composable with existing `columns`, `where`, `indexes`, `constraints`, `triggers` options
- `MaskRule` struct and `pgclone_build_mask_expr()` internal functions for extensible mask strategy architecture
- 15 new pgTAP tests (53 total): email masking, name masking, null masking, hash masking, constant masking, combined masks with WHERE filter
- `test_schema.employees` test fixture table with realistic sensitive data (names, emails, phones, salaries, SSNs)

### Changed
- `CloneOptions` struct extended with `masks[]` array and `num_masks` counter
- `pgclone_parse_options()` handles `"mask"` JSON key with both simple string values (`"email"`) and object values (`{"type":"partial", "prefix":2, "suffix":3}`)
- `pgclone_copy_data()` queries source catalog for column names when masks are active (without explicit column list) to apply per-column mask expressions
- Version bumped to 3.0.0

## [2.2.1]

### Added
- **WHERE clause SQL injection protection**: Two-layer defense against malicious input in the `"where"` JSON option
  - Layer 1: Keyword validation rejects DDL/DML keywords (`DROP`, `INSERT`, `UPDATE`, `DELETE`, `CREATE`, `ALTER`, `TRUNCATE`, etc.) and semicolons before the query is sent
  - Layer 2: Source connection runs inside `BEGIN TRANSACTION READ ONLY` when a WHERE clause is present — PostgreSQL itself blocks any write operations even if validation is bypassed
  - Word-boundary-aware matching avoids false positives on column names like `created_at`, `update_count`, `drop_rate`
- 4 new pgTAP tests (37 total): semicolon rejection, DROP keyword rejection, INSERT keyword rejection, false-positive safety with `created_at` column

## [2.2.0]

### Changed
- **Worker Pool Architecture**: Parallel cloning (`"parallel": N`) now uses a fixed-size worker pool instead of spawning one background worker per table
  - Exactly N workers are launched, each pulling tasks from a shared queue
  - Dynamic load balancing: faster workers automatically handle more tables
  - Resource usage reduced from O(tables) to O(N) for bgworkers and DB connections
  - No longer risk exhausting `max_worker_processes` on large schemas

### Added
- `PgclonePoolQueue` struct in shared memory for task queue management
- `pgclone_pool_worker_main()` background worker entry point
- Pool worker test in `test/test_async.sh` (TEST 8)
- `PGCLONE_MAX_POOL_TASKS` limit (512 tables per pool operation)
- Guard against concurrent pool operations

### Removed
- Per-table background worker launch in parallel mode (replaced by pool)
- Per-table job slot allocation in parallel mode (pool workers share fewer slots)

## [2.1.4]

### Changed
- Local loopback connections now use Unix domain sockets (from `unix_socket_directories` GUC) instead of TCP `127.0.0.1`
- `pg_hba.conf` `trust` entry for `127.0.0.1` is no longer required for async operations — default `local all all peer` is sufficient
- Falls back to TCP `127.0.0.1` automatically if Unix sockets are unavailable

### Fixed
- Security: removed unnecessary `trust` authentication requirement for background worker connections

## [2.1.3]

### Fixed
- Async bgworker: COPY pipeline error handling — failures now logged with `PQerrorMessage`, source COPY result consumed on error path to prevent connection leak
- Async bgworker: `WaitForBackgroundWorkerStartup` added to all async functions — jobs no longer stuck in 'pending' state
- `pgclone_schema_async` parallel mode: fixed hardcoded `jobs[0]` write that corrupted slot 0 in shared memory
- `pgclone_schema_async` parallel mode: parent job now correctly transitions to COMPLETED after child workers finish

### Added
- `pgclone_clear_jobs()` function to free completed/cancelled job slots from shared memory
- Async test suite (`test/test_async.sh`) covering `pgclone_table_async`, `pgclone_schema_async`, `pgclone_progress`, `pgclone_jobs_view`, `pgclone_clear_jobs`
- `CONTRIBUTING.md` — development setup, code guidelines, PR process
- `SECURITY.md` — vulnerability reporting, security considerations
- Documentation restructured: `docs/USAGE.md`, `docs/ASYNC.md`, `docs/TESTING.md`, `docs/ARCHITECTURE.md`, `CHANGELOG.md`

## [2.1.2]

### Added
- Elapsed time column in `pgclone_jobs_view`
- `elapsed_time` field in progress detail output

## [2.1.1]

### Changed
- Visual progress bar in `pgclone_jobs_view` replaces verbose NOTICE messages
- Per-table/per-row NOTICE messages moved to DEBUG1 level

## [2.1.0]

### Added
- `pgclone_jobs_view` — query async job progress as a standard PostgreSQL view
- `pgclone_progress_detail()` — table-returning function for detailed progress

## [2.0.1]

### Added
- `pgclone_database_create()` — create a new database and clone into it
- Automatic pgclone extension installation in the target database
- Idempotent behavior: clones into existing database if it already exists

## [2.0.0]

### Added
- True multi-worker parallel cloning with `"parallel": N` option
- Each table gets its own background worker
- Parent worker monitors child workers via shared memory

### Changed
- Shared memory layout expanded for parallel job tracking

## [1.2.0]

### Added
- Materialized view cloning during schema clone (with `"matviews": false` opt-out)
- Exclusion constraint support (cloned alongside PK, UNIQUE, CHECK, FK)
- Materialized view indexes and data are preserved

## [1.1.0]

### Added
- Selective column cloning with `"columns": [...]` JSON option
- Data filtering with `"where": "..."` JSON option
- Automatic filtering of constraints/indexes referencing excluded columns

## [1.0.0]

### Added
- Async clone operations via background workers (`pgclone_table_async`, `pgclone_schema_async`)
- Job progress tracking (`pgclone_progress`, `pgclone_jobs`)
- Job cancellation (`pgclone_cancel`)
- Job resume from checkpoint (`pgclone_resume`)
- Job cleanup (`pgclone_clear_jobs`)
- Conflict resolution strategies: error, skip, replace, rename

## [0.3.0]

### Added
- Background worker infrastructure
- Shared memory allocation for job state
- `_PG_init` with shmem hooks

## [0.2.0]

### Added
- Index cloning (including expression and partial indexes)
- Constraint cloning (PRIMARY KEY, UNIQUE, CHECK, FOREIGN KEY)
- Trigger cloning with trigger functions
- JSON options format for controlling indexes/constraints/triggers
- Boolean parameter variants (`pgclone_table_ex`, `pgclone_schema_ex`)

## [0.1.0]

### Added
- Initial release
- `pgclone_table()` — clone a single table with or without data
- `pgclone_schema()` — clone an entire schema
- `pgclone_functions()` — clone functions only
- `pgclone_database()` — clone all user schemas
- COPY protocol for fast data transfer
- `pgclone_version()` — version string
- Support for PostgreSQL 14–18
- pgTAP test suite (33 tests)
- Docker Compose multi-version test infrastructure
- GitHub Actions CI pipeline
