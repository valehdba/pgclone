# PgClone

[![CI](https://github.com/valehdba/pgclone/actions/workflows/ci.yml/badge.svg)](https://github.com/valehdba/pgclone/actions/workflows/ci.yml)
[![Postgres 14–18](https://img.shields.io/badge/Postgres-14%E2%80%9318-336791?logo=postgresql&logoColor=white)](https://github.com/valehdba/pgclone)
[![License](https://img.shields.io/badge/License-PostgreSQL-blue.svg)](https://github.com/valehdba/pgclone/blob/main/LICENSE)
[![Version](https://img.shields.io/badge/version-4.1.0-orange)](https://github.com/valehdba/pgclone/releases/tag/v4.1.0)

A PostgreSQL extension that clones databases, schemas, tables, and functions between PostgreSQL instances — directly from SQL. No `pg_dump`, no `pg_restore`, no shell scripts.

## Key Features

- **Clone anything** — tables, schemas, functions, entire databases, roles and permissions
- **Fast data transfer** — uses PostgreSQL COPY protocol
- **Data masking** — anonymize sensitive columns during cloning (email, name, phone, hash, partial, null, random, constant)
- **Auto-discovery** — scan source schemas for sensitive columns and get suggested mask rules
- **Static & dynamic masking** — mask existing tables in-place, or create role-based masked views
- **Async operations** — background workers with progress tracking and visual progress bar
- **Parallel cloning** — multiple background workers for concurrent table cloning
- **Selective cloning** — filter columns and rows with `columns` and `WHERE` options
- **Full DDL support** — indexes, constraints (PK, UNIQUE, CHECK, FK, EXCLUDE), triggers, views, materialized views, sequences
- **Conflict resolution** — error, skip, replace, or rename strategies
- **Cross-version** — tested on PostgreSQL 14, 15, 16, 17, and 18

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
```

## Requirements

- PostgreSQL 14 or later
- PostgreSQL development headers (`postgresql-server-dev-XX`)
- `libpq` development library (`libpq-dev`)
- GCC or compatible C compiler

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

### Enable

```sql
CREATE EXTENSION pgclone;
SELECT pgclone.version();
```

For async operations, add to `postgresql.conf` and restart:

```
shared_preload_libraries = 'pgclone'
```

pgclone uses Unix domain sockets for local loopback connections, so the default `local all all peer` line in `pg_hba.conf` is sufficient — no `trust` entry needed. If Unix sockets are unavailable, pgclone falls back to TCP `127.0.0.1` and appropriate `pg_hba.conf` configuration is required.

## Documentation

| Document | Description |
|----------|-------------|
| [Usage Guide](docs/USAGE.md) | All functions, parameters, data masking, and examples |
| [Async Operations](docs/ASYNC.md) | Background workers, progress tracking, parallel cloning |
| [Testing](docs/TESTING.md) | Test infrastructure, 115 tests across 4 suites, Docker, CI/CD |
| [Manual Testing](docs/TESTING_MANUAL.md) | Step-by-step manual test scenarios for all functionalities |
| [Architecture](docs/ARCHITECTURE.md) | Codebase structure, design decisions, PG version compatibility |
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
- [ ] v4.2.0: Pre-flight validator — connection, space, permissions, version, name-conflict checks before clone (`pgclone.preflight`)
- [ ] v4.3.0: FK-aware referential sampling — sample N rows and follow foreign keys to keep child rows consistent (`pgclone.table_sample`)

## License

[PostgreSQL License](LICENSE)
