# Security Policy

## Supported Versions

Security and bug fixes target the current minor release. Older minor lines receive security fixes for one minor cycle after the next minor is released; older majors are end-of-life.

| Version | Supported |
|---------|-----------|
| 4.3.x   | ✅ Active (current — fixes go here) |
| 4.2.x   | ✅ Security fixes |
| 4.0.x – 4.1.x | ❌ End of life |
| 3.x     | ❌ End of life |
| ≤ 2.x   | ❌ End of life |

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

As of v4.3.1, pgclone re-parses every source `conninfo` via `PQconninfoParse` and reconnects via `PQconnectdbParams` to inject TCP keepalive defaults (issue #9). The conninfo is never serialised back to a single string for logging or storage, and any password parameter is handled exclusively through libpq's parameter array — never echoed by pgclone at `LOG` level or above. If `PQconninfoParse` itself rejects a malformed conninfo, pgclone propagates only libpq's parser message via `ereport(ERROR, ...)`; pgclone does not directly include the raw conninfo in the error. Operators relying on this property should still use `.pgpass` rather than inline passwords, since libpq's own parser message can in rare malformed-syntax cases include short fragments of the input.

### SQL Injection — WHERE Clause

The `"where"` option in JSON parameters is validated against SQL injection patterns (DDL/DML keywords and semicolons are rejected) and executed inside a `READ ONLY` transaction on the source database. This provides two layers of protection:

- Only use `WHERE` filters with trusted input
- Do not pass user-supplied strings directly into the `"where"` option
- The keyword validation and read-only transaction wrapping (v2.2.1) prevent most injection attacks, but defense-in-depth is recommended

### Network Security

pgclone connects to remote PostgreSQL instances using `libpq`. Ensure:

- Firewall rules restrict which hosts can be reached
- Use SSL connections where possible (`sslmode=require` in connection strings)
- Source databases should grant minimal required privileges (read-only access is sufficient for cloning)

### Shared Memory

Async job state is stored in PostgreSQL shared memory. Job metadata (schema names, table names, progress) is visible to all database users who can call `pgclone_progress()` or query `pgclone_jobs_view`. This is expected behavior but should be considered in multi-tenant environments.
