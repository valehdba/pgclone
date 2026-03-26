# pgx_clone

A PostgreSQL extension for cloning databases, schemas, tables, and functions between PostgreSQL hosts.

## Requirements

- PostgreSQL 14 or later
- PostgreSQL development headers (`postgresql-server-dev-XX`)
- `libpq` development library (`libpq-dev`)
- GCC or compatible C compiler

## Installation

### From source

```bash
# Install build dependencies (Debian/Ubuntu)
sudo apt-get install postgresql-server-dev-16 libpq-dev build-essential

# Build and install
make
sudo make install
```

### Enable the extension

```sql
CREATE EXTENSION pgx_clone;
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
