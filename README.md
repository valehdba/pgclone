# PgClone

[![CI](https://github.com/valehdba/pgclone/actions/workflows/ci.yml/badge.svg)](https://github.com/valehdba/pgclone/actions/workflows/ci.yml)
[![Postgres 14–18](https://img.shields.io/badge/Postgres-14%E2%80%9318-336791?logo=postgresql&logoColor=white)](https://github.com/valehdba/pgclone)
[![License](https://img.shields.io/badge/License-PostgreSQL-blue.svg)](https://github.com/valehdba/pgclone/blob/main/LICENSE)
[![Version](https://img.shields.io/badge/version-4.5.0-orange)](https://github.com/valehdba/pgclone/releases/tag/v4.5.0)

A PostgreSQL extension that clones databases, schemas, tables, and functions between PostgreSQL instances — directly from SQL. No `pg_dump`, no `pg_restore`, no shell scripts. Since v4.5.0 it can also pull a table straight out of an **Oracle** database, since v4.6.0 out of **Microsoft SQL Server**, and since v4.7.0 out of **MySQL / MariaDB**, into PostgreSQL.

## Key Features

- **Clone anything** - tables, schemas, functions, entire databases, roles and permissions
- **Oracle → PostgreSQL** - clone an Oracle table (structure, NOT NULL, PK/UNIQUE, data) with automatic type mapping, a `dry_run` DDL preview and an all-or-nothing transaction (optional OCI build)
- **SQL Server → PostgreSQL** - clone a SQL Server table or view (structure, NOT NULL, PK/UNIQUE, data) with automatic type mapping, a `dry_run` DDL preview and an all-or-nothing transaction (optional FreeTDS build)
- **MySQL / MariaDB → PostgreSQL** - clone a MySQL table or view (structure, NOT NULL, PK/UNIQUE, data) with automatic type mapping, a consistent InnoDB snapshot, a `dry_run` DDL preview and an all-or-nothing transaction (optional MySQL client build)
- **Fast data transfer** - uses PostgreSQL COPY protocol
- **Data masking** - anonymize sensitive columns during cloning (email, name, phone, hash, partial, null, random, constant)
- **Auto-discovery** - scan source schemas for sensitive columns and get suggested mask rules
- **Static & dynamic masking** — mask existing tables in-place, or create role-based masked views
- **Async operations** - background workers with progress tracking and visual progress bar
- **Parallel cloning** - multiple background workers for concurrent table cloning
- **Selective cloning** - filter columns and rows with `columns` and `WHERE` options
- **Full DDL support** - indexes, constraints (PK, UNIQUE, CHECK, FK, EXCLUDE), triggers, views, materialized views, sequences, generated columns
- **Conflict resolution** - error, skip, replace, or rename strategies
- **Cross-version** - tested on PostgreSQL 14, 15, 16, 17, 18, and 19

## Quick Start

```bash
# Build and install
git clone https://github.com/valehdba/pgclone.git
cd pgclone
make
sudo make install
```

```sql
-- Enable the extension
CREATE EXTENSION pgclone;

-- Clone a table
SELECT pgclone.table(
    'host=source-server dbname=mydb user=postgres password=secret',
    'public', 'customers', true
);

-- Clone an entire schema
SELECT pgclone.schema(
    'host=source-server dbname=mydb user=postgres password=secret',
    'sales', true
);

-- Clone a full database
SELECT pgclone.database(
    'host=source-server dbname=mydb user=postgres password=secret',
    true
);

-- Clone a table from Oracle (needs the optional Oracle build, see Installation)
SELECT pgclone.table_from_oracle(
    'host=ora-server port=1521 service=ORCLPDB1 user=scott password=tiger',
    'HR', 'EMPLOYEES'
);

-- Clone a table from SQL Server (needs the optional FreeTDS build, see Installation)
SELECT pgclone.table_from_sqlserver(
    'host=mssql-server port=1433 database=Sales user=sa password=secret',
    'dbo', 'Customers'
);

-- Clone a table from MySQL / MariaDB (needs the optional MySQL client build, see Installation)
SELECT pgclone.table_from_mysql(
    'host=mysql-server port=3306 user=root password=secret',
    'shop', 'orders'
);
```

## Requirements

- PostgreSQL 14 or later
- PostgreSQL development headers (`postgresql-server-dev-XX`)
- `libpq` development library (`libpq-dev`)
- GCC or compatible C compiler
- *Optional, for Oracle cloning:* Oracle Instant Client **Basic/Basic Light + SDK** (provides `oci.h` and `libclntsh`)
- *Optional, for SQL Server cloning:* FreeTDS development files (`freetds-dev` / `freetds-devel` / `brew install freetds`; provides `sybdb.h` and `libsybdb`)
- *Optional, for MySQL / MariaDB cloning:* a MySQL C client library with headers (`libmariadb-dev` / `mariadb-connector-c-devel` / `brew install mariadb-connector-c`, or libmysqlclient)

## Installation

### Debian / Ubuntu

```bash
sudo apt-get install postgresql-server-dev-18 libpq-dev build-essential
```

### RHEL / CentOS / Rocky / AlmaLinux

```bash
sudo dnf install postgresql18-devel libpq-devel gcc make
```

### macOS (Homebrew)

```bash
brew install postgresql@18
```

### Build

```bash
make
sudo make install

# For a specific PostgreSQL version:
make PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config
sudo make install PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config
```

### Build with Oracle support (optional)

`pgclone.table_from_oracle()` talks to Oracle through OCI, so it is compiled in only on request. A normal build is unaffected and the function then raises a clear "built without Oracle support" error.

```bash
make WITH_ORACLE=1 ORACLE_HOME=/opt/oracle/instantclient_23_5
sudo make install WITH_ORACLE=1 ORACLE_HOME=/opt/oracle/instantclient_23_5
```

The PostgreSQL server process must be able to load `libclntsh` at runtime (`LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH` or ldconfig). See [Cloning a Table from Oracle](docs/USAGE.md#cloning-a-table-from-oracle-v450) for usage, type mapping and limitations.

### Build with SQL Server support (optional)

`pgclone.table_from_sqlserver()` talks to SQL Server through FreeTDS (DB-Library), so it is compiled in only on request. A normal build is unaffected and the function then raises a clear "built without SQL Server support" error.

```bash
# Debian/Ubuntu: sudo apt-get install freetds-dev    macOS: brew install freetds
make WITH_SQLSERVER=1
sudo make install WITH_SQLSERVER=1

# FreeTDS in a non-standard prefix (e.g. Homebrew on Apple Silicon)
make WITH_SQLSERVER=1 FREETDS_HOME=/opt/homebrew
```

See [Cloning a Table from SQL Server](docs/USAGE.md#cloning-a-table-from-sql-server-v460) for usage, type mapping and limitations.

### Build with MySQL support (optional)

`pgclone.table_from_mysql()` talks to MySQL / MariaDB through the standard C client API, so it is compiled in only on request. A normal build is unaffected and the function then raises a clear "built without MySQL support" error.

```bash
# Debian/Ubuntu: sudo apt-get install libmariadb-dev    macOS: brew install mariadb-connector-c
make WITH_MYSQL=1
sudo make install WITH_MYSQL=1

# pick a specific client library
make WITH_MYSQL=1 MYSQL_CONFIG=/opt/homebrew/opt/mariadb-connector-c/bin/mariadb_config
```

See [Cloning a Table from MySQL](docs/USAGE.md#cloning-a-table-from-mysql-v470) for usage, type mapping and limitations.

### Troubleshooting: `make install` fails with `llvm-lto: No such file or directory`

If PostgreSQL was itself built `--with-llvm` (check `pg_config --configure | grep llvm`), PGXS also builds a JIT bitcode file for every extension and links it with `llvm-lto`. Some LLVM packages (seen on RHEL/CentOS with a separately-installed `llvm-toolset`, e.g. `llvm20`) don't ship that specific tool, so the build fails like:

```
/bin/sh: /usr/lib64/llvm20/bin/llvm-lto: No such file or directory
make: *** [.../pgxs.mk:241: install] Error 127
```

This is an LLVM/PGXS toolchain mismatch, not a pgclone issue — bitcode is only used for JIT-inlining expressions across a function's internals, which has no meaningful benefit for an I/O-bound cloning extension. Skip it with `with_llvm=no`:

```bash
make with_llvm=no
sudo make with_llvm=no install
```

### Enable

```sql
CREATE EXTENSION pgclone;
SELECT pgclone.version();
```

For async operations, add to `postgresql.conf` and restart:

```
shared_preload_libraries = 'pgclone'
```

### Upgrading

```bash
make clean && make && sudo make install
# restart PostgreSQL so the new pgclone library is loaded
sudo systemctl restart postgresql            # or your platform's equivalent
```

```sql
ALTER EXTENSION pgclone UPDATE;
SELECT pgclone.version();
```

**Restart PostgreSQL before `ALTER EXTENSION pgclone UPDATE`** when pgclone is in `shared_preload_libraries`. The running server keeps the old shared library mapped after `make install` replaces it on disk, so the update script can fail with `could not find function "..." in file ".../pgclone.so"` until the server is restarted.

pgclone uses Unix domain sockets for local loopback connections, so the default `local all all peer` line in `pg_hba.conf` is sufficient — no `trust` entry needed. If Unix sockets are unavailable, pgclone falls back to TCP `127.0.0.1` and appropriate `pg_hba.conf` configuration is required.

## Documentation

| Document | Description |
|----------|-------------|
| [Usage Guide](docs/USAGE.md) | All functions, parameters, data masking, Oracle, SQL Server and MySQL cloning, and examples |
| [Async Operations](docs/ASYNC.md) | Background workers, progress tracking, parallel cloning |
| [Testing](docs/TESTING.md) | Test infrastructure, pgTAP and integration suites (incl. Oracle Free, SQL Server and MySQL), Docker, CI/CD |
| [Manual Testing](docs/TESTING_MANUAL.md) | Step-by-step manual test scenarios for all functionalities |
| [Architecture](docs/ARCHITECTURE.md) | Codebase structure, design decisions, Oracle/OCI, SQL Server/FreeTDS and MySQL client isolation, PG version compatibility |
| [Changelog](CHANGELOG.md) | Version history and release notes |

## Roadmap

- [x] v0.1.0: Clone tables, schemas, functions, databases
- [x] v0.2.0: Indexes, constraints, triggers
- [x] v0.3.0: Async background workers with progress tracking
- [x] v1.0.0: Resume support and conflict resolution
    - [x] v1.1.0: Selective column cloning and data filtering
    - [x] v1.2.0: Materialized views and exclusion constraints
- [x] v2.0.0: True multi-worker parallel cloning
    - [x] v2.0.1: `pgclone.database_create()` — create + clone database
    - [x] v2.1.0: Progress tracking view (`pgclone.jobs_view`)
    - [x] v2.1.1: Visual progress bar
    - [x] v2.1.3: Elapsed time tracking
    - [x] v2.1.4: Unix domain socket auth (no more pg_hba.conf trust requirement)
    - [x] v2.2.0: Worker pool (fixed pool size instead of one bgworker per table)
    - [x] v2.2.1: Read-only transaction for WHERE clause (SQL injection protection)
- [x] v3.0.0: Data anonymization / masking
    - [x] v3.1.0: Auto-Discovery of Sensitive Data
    - [x] v3.2.0: Applying Static Data Masking to cloned data
    - [x] v3.3.0: Applying Dynamic Data Masking to cloned data
    - [x] v3.4.0: Clone roles with permissions and passwords
- [x] v3.5.0: Clone verification - compare row counts across source and target
- [x] v3.6.0: GDPR/Compliance masking report
- [x] v4.0.0: Schema namespace — all functions under `pgclone` schema (`pgclone.table()`, `pgclone.schema()`, etc.)
- [x] v4.1.0: Schema diff — DDL drift detection between source and target (`pgclone.diff`)
- [x] v4.2.0: Pre-flight validator — connection, space, permissions, version, name-conflict checks before clone (`pgclone.preflight`)
- [x] v4.3.0: Consistent-snapshot clones — every clone reads the source under one shared `REPEATABLE READ READ ONLY` snapshot (sync, async, and parallel pool); cross-table FK consistency on a live source is now guaranteed (same model as `pg_dump -j`)
- [x] v4.4.0: Schema/database-level in-line masking (`"masks"`) and regex table subset filters (`"tables"` / `"exclude_tables"`) — discussion #16
- [x] v4.4.1: Type-aware masking — a mask that a column's type cannot store (e.g. `random_int` on a `boolean`) is skipped with a warning instead of aborting the clone; `discover_sensitive` no longer suggests incompatible masks — issue #17
- [x] v4.4.2: Constraint- and length-aware masking — masks are clamped to a column's length and skipped when they would overflow, feed a bad literal to a numeric column, or violate a NOT NULL / UNIQUE / FOREIGN KEY constraint; UNIQUE/PK and NOT NULL sensitive columns are steered to `hash` — issue #18
- [x] v4.4.3: `"conflict"` (`error`/`skip`/`replace`/`rename`) is now honored by the synchronous clone path (`pgclone.table()`/`pgclone.schema()`/`pgclone.database()`), not just the async job path — issue #22
- [x] v4.4.4: Auto-discovery of bank account numbers/IBAN; `pgclone.masking_report()` gains a `(conninfo, schema)` overload to audit a remote source directly — issue #23
- [x] v4.5.0: Clone a table from Oracle into PostgreSQL — `pgclone.table_from_oracle()` with Oracle→PG type mapping, `dry_run`, CLOB/RAW support and an optional OCI build (`WITH_ORACLE=1`); verified in CI against Oracle Free
- [x] v4.6.0: Clone a table from Microsoft SQL Server into PostgreSQL — `pgclone.table_from_sqlserver()` with SQL Server→PG type mapping, `dry_run`, `NVARCHAR(MAX)`/`VARBINARY`/`UNIQUEIDENTIFIER` support and an optional FreeTDS build (`WITH_SQLSERVER=1`); CI job against SQL Server 2022
- [x] v4.7.0: Clone a table from MySQL / MariaDB into PostgreSQL — `pgclone.table_from_mysql()` with MySQL→PG type mapping, `dry_run`, unsigned/`BIT`/`JSON`/`ENUM`/`SET`/`BLOB` support, zero-date handling and an optional client build (`WITH_MYSQL=1`); CI job against MySQL 8.0
- [x] v4.8.0: `"conflict": "ignore"`, partitioned-table cloning (`PARTITION BY` / `PARTITION OF`), generated columns, `table_async` sequences, PG17+ worker SIGTERM fix — see [CHANGELOG](CHANGELOG.md)
- [x] v4.9.0: `"on_error": "continue"` for `pgclone.schema()` / `pgclone.database()` — see [CHANGELOG](CHANGELOG.md)
- [x] v4.10.0: PostgreSQL 19 support (build fix, CI matrix 14–19) — see [CHANGELOG](CHANGELOG.md)
- [ ] v4.11.0: FK-aware referential sampling — sample N rows and follow foreign keys to keep child rows consistent (`pgclone.table_sample`)

## License

[PostgreSQL License](LICENSE)
