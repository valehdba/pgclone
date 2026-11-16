/*
 * pgclone_tds.h - thin, PostgreSQL-header-free wrapper around FreeTDS
 * DB-Library (sybdb.h) for talking to Microsoft SQL Server.
 *
 * sybdb.h cannot be included next to postgres.h (both define BOOL, STATUS,
 * TRUE/FALSE ...), so every FreeTDS call lives in pgclone_tds.c behind this
 * plain-C interface. When pgclone is built without WITH_SQLSERVER=1 every
 * function here is a stub that reports "built without SQL Server support".
 *
 * All functions return 0 on success and -1 on failure (tds_fetch: 1 = row,
 * 0 = end of data, -1 = error), writing a message into errbuf.
 *
 * pgclone always converts every column to text on the SQL Server side, so
 * values come back as character data in UTF-8.
 */
#ifndef PGCLONE_TDS_H
#define PGCLONE_TDS_H

#include <stddef.h>

typedef struct TdsConn   TdsConn;
typedef struct TdsCursor TdsCursor;

extern int  tds_is_available(void);

/* encrypt: -1 = driver default (freetds.conf), 0 = off, 1 = require. */
extern TdsConn *tds_connect(const char *host, const char *port,
                            const char *database, const char *user,
                            const char *password, int encrypt,
                            char *errbuf, size_t errlen);
extern void tds_disconnect(TdsConn *conn);

/* Run a batch that returns no rows (SET ..., BEGIN TRANSACTION). */
extern int  tds_exec(TdsConn *conn, const char *sql, char *errbuf, size_t errlen);

/* Open a SELECT that must return exactly ncols columns. Only one cursor may
 * be open per connection at a time. */
extern TdsCursor *tds_open(TdsConn *conn, const char *sql, int ncols,
                           char *errbuf, size_t errlen);
extern int  tds_fetch(TdsCursor *cur, char *errbuf, size_t errlen);

/* Value of column `col` (0-based) in the current row; NULL for SQL NULL.
 * Not NUL-terminated - use *len. Valid until the next tds_fetch(). */
extern const char *tds_value(TdsCursor *cur, int col, size_t *len);
extern void tds_close(TdsCursor *cur);

#endif /* PGCLONE_TDS_H */
