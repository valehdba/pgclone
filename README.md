# PgClone

[![CI](https://github.com/valehdba/pgclone/actions/workflows/ci.yml/badge.svg)](https://github.com/valehdba/pgclone/actions/workflows/ci.yml)
[![Postgres 14–18](https://img.shields.io/badge/Postgres-14%E2%80%9318-336791?logo=postgresql&logoColor=white)](https://github.com/valehdba/pgclone)
[![License](https://img.shields.io/badge/License-PostgreSQL-blue.svg)](https://github.com/valehdba/pgclone/blob/main/LICENSE)
[![Version](https://img.shields.io/badge/version-2.1.2-orange)](https://github.com/valehdba/pgclone/releases/tag/v2.1.2)

A PostgreSQL extension written in C that clones databases, schemas, tables, and functions between PostgreSQL instances — directly from SQL. No `pg_dump`, no `pg_restore`, no shell scripts.

## Key Features

- **Clone anything** — tables, schemas, functions, entire databases
- **Fast data transfer** — uses PostgreSQL COPY protocol
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
SELECT pgclone_table(
    'host=source-server dbname=mydb user=postgres password=secret',
    'public', 'customers', true
);

-- Clone an entire schema
SELECT pgclone_schema(
    'host=source-server dbname=mydb user=postgres password=secret',
    'sales', true
);

-- Clone a full database
SELECT pgclone_database(
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
SELECT pgclone_version();
```

For async operations, add to `postgresql.conf` and restart:

```
shared_preload_libraries = 'pgclone'
```

## Documentation

| Document | Description |
|----------|-------------|
| [Usage Guide](docs/USAGE.md) | All functions, parameters, and examples |
| [Async Operations](docs/ASYNC.md) | Background workers, progress tracking, parallel cloning |
| [Testing](docs/TESTING.md) | Test infrastructure, pgTAP, Docker, CI/CD |
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
- [x] v2.0.1: `pgclone_database_create()` — create + clone database
- [x] v2.1.0: Progress tracking view (`pgclone_jobs_view`)
- [x] v2.1.1: Visual progress bar
- [x] v2.1.2: Elapsed time tracking
- [ ] v2.2.0: Worker pool (fixed pool size instead of one bgworker per table)
- [ ] v2.2.1: Read-only transaction for WHERE clause (SQL injection protection)
- [ ] v3.0.0: Data anonymization / masking
- [ ] v4.0.0: Copy-on-Write (CoW) mode for local cloning

## License

PostgreSQL License
