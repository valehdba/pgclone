# Contributing to PgClone

Thank you for your interest in contributing to PgClone! This guide will help you get started.

## Getting Started

1. Fork the repository at [github.com/valehdba/pgclone](https://github.com/valehdba/pgclone)
2. Clone your fork locally
3. Create a feature branch from `main`

```bash
git clone https://github.com/YOUR_USERNAME/pgclone.git
cd pgclone
git checkout -b feature/your-feature-name
```

## Development Setup

### Prerequisites

- PostgreSQL 14 or later with development headers
- `libpq-dev`
- GCC or compatible C compiler
- Docker and Docker Compose (for testing)

### Build

```bash
make PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config
sudo make install PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config
```

### Run Tests

pgclone uses Docker Compose to test across PostgreSQL 14–18:

```bash
# Run tests on PostgreSQL 17
docker compose down -v
docker compose up source-db -d
docker compose run --rm test-pg17

# Filter output
docker compose run --rm test-pg17 2>&1 | grep -E "(ok|not ok|WARNING|ERROR|PASSED|FAILED)"
```

All tests must pass on all supported PostgreSQL versions before a PR can be merged.

## Project Structure

```
src/pgclone.c       — Core extension (table, schema, database clone)
src/pgclone_bgw.c   — Background worker (async operations)
src/pgclone_bgw.h   — Shared definitions and structs
sql/pgclone--*.sql   — SQL function definitions per version
test/                — Test suite (pgTAP + shell scripts)
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for detailed design documentation.

## How to Contribute

### Reporting Bugs

Open an issue with:
- PostgreSQL version
- pgclone version (`SELECT pgclone_version();`)
- Steps to reproduce
- Expected vs actual behavior
- Server logs if relevant

### Submitting Changes

1. Create a branch: `feature/your-feature` or `fix/bug-description`
2. Write your code following the patterns in the existing codebase
3. Add tests — pgTAP tests in `test/pgclone_test.sql` or shell tests in `test/`
4. Ensure all tests pass on at least PG 17 locally
5. Open a Pull Request against `main`

### Code Guidelines

- **C style:** Follow the existing code style in `pgclone.c` — PostgreSQL naming conventions, `ereport` for errors, `PG_TRY/PG_CATCH` for cleanup
- **Version guards:** Use `#if PG_VERSION_NUM >= XXXXXX` preprocessor guards when using version-specific APIs
- **Resource cleanup:** Every `PQconnectdb` must have a matching `PQfinish`, every `PQexec` result must be `PQclear`-ed, including in error paths
- **SQL files:** New functions need a corresponding `sql/pgclone--X.Y.Z.sql` file
- **Tests:** New features must include tests. Increment the `plan()` count in `pgclone_test.sql` if adding pgTAP tests

### Branch Naming

- `feature/description` — new functionality
- `fix/description` — bug fixes
- CI runs on `main`, `fix/**`, and `feature/**` branches

## Questions?

Open an issue on GitHub — we're happy to help.
