/*
 * pgclone_oci.h - thin, PostgreSQL-header-free wrapper around Oracle OCI.
 *
 * oci.h cannot be included next to postgres.h (both define "text"), so all
 * OCI calls live in pgclone_oci.c behind this plain-C interface. When pgclone
 * is built without WITH_ORACLE=1 every function here is a stub that reports
 * "built without Oracle support".
 *
 * All functions return 0 on success and -1 on failure (ora_fetch: 1 = row,
 * 0 = end of data, -1 = error), writing a message into errbuf.
 */
#ifndef PGCLONE_OCI_H
#define PGCLONE_OCI_H

#include <stddef.h>

typedef struct OraConn   OraConn;
typedef struct OraCursor OraCursor;

#define ORA_COL_TEXT   0    /* fetched as character data (SQLT_CHR) */
#define ORA_COL_BINARY 1    /* fetched as raw bytes      (SQLT_BIN) */
#define ORA_COL_CLOB   2    /* CLOB/NCLOB read in full via a LOB locator;
                             * bufsz is the read-chunk size, not a limit */

extern int  ora_is_available(void);

/* connect_string: "//host:port/service" or a tnsnames alias. */
extern OraConn *ora_connect(const char *connect_string, const char *user,
                            const char *password, char *errbuf, size_t errlen);
extern void ora_disconnect(OraConn *conn);

/* Run a statement that returns no rows (ALTER SESSION, SET TRANSACTION). */
extern int  ora_exec(OraConn *conn, const char *sql, char *errbuf, size_t errlen);

/* Open a SELECT. kinds[i] / bufsz[i] describe how to fetch column i
 * (bufsz in bytes). Rows are then pulled with ora_fetch(). */
extern OraCursor *ora_open(OraConn *conn, const char *sql, int ncols,
                           const int *kinds, const unsigned *bufsz,
                           char *errbuf, size_t errlen);
extern int  ora_fetch(OraCursor *cur, char *errbuf, size_t errlen);

/* Value of column `col` in the current row; NULL for SQL NULL.
 * Valid until the next ora_fetch(). */
extern const char *ora_value(OraCursor *cur, int col, size_t *len);
extern void ora_close(OraCursor *cur);

#endif /* PGCLONE_OCI_H */
