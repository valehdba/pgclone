/*
 * pgclone_myc.h - thin, PostgreSQL-header-free wrapper around the MySQL C
 * client API (libmysqlclient or MariaDB Connector/C, both expose <mysql.h>).
 *
 * mysql.h cannot reliably be included next to postgres.h (conflicting
 * typedefs such as "bool"), and its function names (mysql_*) would be easy to
 * confuse with ours, so every client call lives in pgclone_myc.c behind this
 * plain-C interface using the "myc_" prefix. When pgclone is built without
 * WITH_MYSQL=1 every function here is a stub that reports "built without
 * MySQL support".
 *
 * All functions return 0 on success and -1 on failure (myc_fetch: 1 = row,
 * 0 = end of data, -1 = error), writing a message into errbuf.
 *
 * pgclone reads every column as text (the MySQL text protocol), in UTF-8.
 */
#ifndef PGCLONE_MYC_H
#define PGCLONE_MYC_H

#include <stddef.h>

typedef struct MycConn   MycConn;
typedef struct MycCursor MycCursor;

extern int  myc_is_available(void);

/* ssl: -1 = client default, 0 = disabled, 1 = required. */
extern MycConn *myc_connect(const char *host, const char *port,
                            const char *user, const char *password,
                            int ssl, char *errbuf, size_t errlen);
/* Safe while a cursor is still open (abandons the rest of its result). */
extern void myc_disconnect(MycConn *conn);

/* Run a statement that returns no rows (SET ..., START TRANSACTION). */
extern int  myc_exec(MycConn *conn, const char *sql, char *errbuf, size_t errlen);

/* Open a SELECT that must return exactly ncols columns. Rows are streamed
 * (not buffered). Only one cursor may be open per connection at a time. */
extern MycCursor *myc_open(MycConn *conn, const char *sql, int ncols,
                           char *errbuf, size_t errlen);
extern int  myc_fetch(MycCursor *cur, char *errbuf, size_t errlen);

/* Value of column `col` (0-based) in the current row; NULL for SQL NULL.
 * Not NUL-terminated - use *len. Valid until the next myc_fetch(). */
extern const char *myc_value(MycCursor *cur, int col, size_t *len);
extern void myc_close(MycCursor *cur);

#endif /* PGCLONE_MYC_H */
