# pgx_clone

A PostgreSQL extension written in C for cloning databases, schemas, tables, and functions between PostgreSQL hosts — directly from SQL.

## Requirements

- PostgreSQL 14 or later (tested on 14, 15, 16, 17, 18)
- PostgreSQL development headers (`postgresql-server-dev-XX`)
- `libpq` development library (`libpq-dev`)
- GCC or compatible C compiler
- `make` and `pg_config` in your PATH

## Installation

### Install build dependencies

#### Debian / Ubuntu

```bash
sudo apt-get install postgresql-server-dev-18 libpq-dev build-essential
```

#### RHEL / CentOS / Rocky / AlmaLinux

```bash
sudo dnf install postgresql18-devel libpq-devel gcc make
```

#### macOS (Homebrew)

```bash
brew install postgresql@18
```

### Build and install

```bash
git clone https://github.com/valehdba/pgx_clone.git
cd pgx_clone
make
sudo make install
```

If you have multiple PostgreSQL versions installed, specify which one:

```bash
make PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config
sudo make install PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config
```

### Enable the extension

```sql
-- Connect to your target database
CREATE EXTENSION pgx_clone;

-- Verify installation
SELECT pgx_clone_version();
```

## Usage

### Clone a single table (with data)

```sql
SELECT pgx_clone_table(
    'host=source-server dbname=mydb user=postgres password=secret',
    'public',           -- schema name
    'customers',        -- table name
    true                -- include data (default: true)
);
```

### Clone a table (structure only, no data)

```sql
SELECT pgx_clone_table(
    'host=source-server dbname=mydb user=postgres',
    'public',
    'customers',
    false
);
```

### Clone a table with a different name on target

```sql
SELECT pgx_clone_table(
    'host=source-server dbname=mydb user=postgres',
    'public',
    'customers',        -- source table name
    true,               -- include data
    'customers_backup'  -- target table name (will be created as customers_backup)
);
```

### Clone an entire schema (tables + views + functions + sequences)

```sql
SELECT pgx_clone_schema(
    'host=source-server dbname=mydb user=postgres',
    'sales',            -- schema to clone
    true                -- include table data
);
```

### Clone only functions from a schema

```sql
SELECT pgx_clone_functions(
    'host=source-server dbname=mydb user=postgres',
    'utils'             -- schema containing functions
);
```

### Clone an entire database (all user schemas)

```sql
SELECT pgx_clone_database(
    'host=source-server dbname=mydb user=postgres',
    true                -- include data
);
```

### Check version

```sql
SELECT pgx_clone_version();
-- Returns: pgx_clone 0.1.0
```

## Connection String Format

The `source_conninfo` parameter uses standard PostgreSQL connection strings:

```
host=hostname dbname=database user=username password=password port=5432
```

Or URI format:

```
postgresql://username:password@hostname:5432/database
```

## Security Notes

- This extension requires **superuser** privileges to install and use
- Connection strings may contain passwords — consider using `.pgpass` files
  or `PGPASSFILE` environment variable instead
- The extension connects to remote hosts using `libpq` — ensure network
  connectivity and firewall rules allow the connection

## Current Limitations (v0.1.0)

- No support for indexes, constraints (foreign keys, checks), or triggers yet
- Data transfer uses row-by-row INSERT (not COPY) — slower for large tables
- No progress tracking for long operations
- No parallel cloning support yet

## Roadmap

- [ ] v0.2.0: Clone indexes, constraints, and triggers
- [ ] v0.3.0: Use COPY protocol for fast data transfer
- [ ] v0.4.0: Background worker for async operations with progress tracking
- [ ] v0.5.0: Selective column cloning and data filtering
- [ ] v1.0.0: Parallel cloning, resume support, and conflict resolution

## License

PostgreSQL License
