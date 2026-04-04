# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| 2.1.x   | ✅ Active |
| 2.0.x   | ✅ Security fixes |
| 1.x     | ❌ End of life |
| 0.x     | ❌ End of life |

## Reporting a Vulnerability

If you discover a security vulnerability in PgClone, please report it responsibly:

1. **Do not** open a public GitHub issue for security vulnerabilities
2. Email the maintainer directly or use GitHub's private vulnerability reporting feature
3. Include a clear description of the vulnerability and steps to reproduce

We will acknowledge your report within 48 hours and provide a timeline for a fix.

## Security Considerations

PgClone operates with **superuser** privileges and handles database connections. Users should be aware of the following:

### Connection Strings

Connection strings passed to pgclone functions may contain passwords in plaintext. To avoid exposing credentials:

- Use `.pgpass` files or the `PGPASSFILE` environment variable instead of inline passwords
- Restrict access to pgclone functions to trusted users only
- Avoid logging connection strings in application logs

### SQL Injection — WHERE Clause

The `"where"` option in JSON parameters is passed directly to SQL queries on the **source** database. This is by design for flexibility, but means:

- Only use `WHERE` filters with trusted input
- Do not pass user-supplied strings directly into the `"where"` option
- A future version (v2.2.1) will add read-only transaction wrapping for additional protection

### Network Security

pgclone connects to remote PostgreSQL instances using `libpq`. Ensure:

- Firewall rules restrict which hosts can be reached
- Use SSL connections where possible (`sslmode=require` in connection strings)
- Source databases should grant minimal required privileges (read-only access is sufficient for cloning)

### Shared Memory

Async job state is stored in PostgreSQL shared memory. Job metadata (schema names, table names, progress) is visible to all database users who can call `pgclone_progress()` or query `pgclone_jobs_view`. This is expected behavior but should be considered in multi-tenant environments.
