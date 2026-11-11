/*
 * pgclone_oci.c - Oracle OCI access for pgclone.table_from_oracle().
 *
 * Deliberately does NOT include any PostgreSQL header (see pgclone_oci.h).
 * Compiled in full only with -DPGCLONE_WITH_ORACLE (make WITH_ORACLE=1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pgclone_oci.h"

#ifndef PGCLONE_WITH_ORACLE

/* ---------------- stub build: no Oracle client available ---------------- */
static int
ora_unsupported(char *errbuf, size_t errlen)
{
    snprintf(errbuf, errlen,
             "pgclone was built without Oracle support "
             "(rebuild with: make WITH_ORACLE=1 ORACLE_HOME=/path/to/instantclient)");
    return -1;
}

int ora_is_available(void) { return 0; }
OraConn *ora_connect(const char *c, const char *u, const char *p, char *e, size_t l)
{ (void) c; (void) u; (void) p; ora_unsupported(e, l); return NULL; }
void ora_disconnect(OraConn *conn) { (void) conn; }
int ora_exec(OraConn *conn, const char *sql, char *e, size_t l)
{ (void) conn; (void) sql; return ora_unsupported(e, l); }
OraCursor *ora_open(OraConn *conn, const char *sql, int n, const int *k,
                    const unsigned *b, char *e, size_t l)
{ (void) conn; (void) sql; (void) n; (void) k; (void) b; ora_unsupported(e, l); return NULL; }
int ora_fetch(OraCursor *cur, char *e, size_t l)
{ (void) cur; return ora_unsupported(e, l); }
const char *ora_value(OraCursor *cur, int col, size_t *len)
{ (void) cur; (void) col; if (len) *len = 0; return NULL; }
void ora_close(OraCursor *cur) { (void) cur; }

#else /* PGCLONE_WITH_ORACLE */

#include <oci.h>

#define ORA_PREFETCH_ROWS 1000
#define PGCLONE_AL32UTF8_ID 873   /* Oracle charset id of AL32UTF8 */

struct OraConn
{
    OCIEnv     *env;
    OCIError   *err;
    OCISvcCtx  *svc;
    int         logged_on;
};

typedef struct OraColBuf
{
    char       *buf;
    unsigned    bufsz;
    sb2         ind;
    ub2         len;
    ub2         rc;
} OraColBuf;

struct OraCursor
{
    OraConn    *conn;
    OCIStmt    *stmt;
    int         ncols;
    OraColBuf  *cols;
};

int ora_is_available(void) { return 1; }

static void
ora_geterr(OraConn *conn, char *errbuf, size_t errlen, const char *what)
{
    text    msg[512];
    sb4     code = 0;

    msg[0] = '\0';
    if (conn && conn->err)
        OCIErrorGet(conn->err, 1, NULL, &code, msg, sizeof(msg), OCI_HTYPE_ERROR);
    snprintf(errbuf, errlen, "%s failed: %s", what, (char *) msg);
}

#define OK(rc) ((rc) == OCI_SUCCESS || (rc) == OCI_SUCCESS_WITH_INFO)

OraConn *
ora_connect(const char *connect_string, const char *user, const char *password,
            char *errbuf, size_t errlen)
{
    OraConn *conn = calloc(1, sizeof(OraConn));
    sword    rc;

    if (!conn)
    {
        snprintf(errbuf, errlen, "out of memory");
        return NULL;
    }

    /* AL32UTF8 client character set: PostgreSQL side is UTF-8. */
    rc = OCIEnvNlsCreate(&conn->env, OCI_THREADED | OCI_OBJECT, NULL, NULL, NULL,
                         NULL, 0, NULL, PGCLONE_AL32UTF8_ID, PGCLONE_AL32UTF8_ID);
    if (rc != OCI_SUCCESS || !conn->env)
    {
        snprintf(errbuf, errlen, "OCIEnvNlsCreate failed (is the Oracle client library usable?)");
        free(conn);
        return NULL;
    }
    OCIHandleAlloc(conn->env, (void **) &conn->err, OCI_HTYPE_ERROR, 0, NULL);

    rc = OCILogon(conn->env, conn->err, &conn->svc,
                  (const OraText *) user, (ub4) strlen(user),
                  (const OraText *) password, (ub4) strlen(password),
                  (const OraText *) connect_string, (ub4) strlen(connect_string));
    if (!OK(rc))
    {
        ora_geterr(conn, errbuf, errlen, "Oracle logon");
        ora_disconnect(conn);
        return NULL;
    }
    conn->logged_on = 1;
    return conn;
}

void
ora_disconnect(OraConn *conn)
{
    if (!conn)
        return;
    if (conn->logged_on)
        OCILogoff(conn->svc, conn->err);
    if (conn->err)
        OCIHandleFree(conn->err, OCI_HTYPE_ERROR);
    if (conn->env)
        OCIHandleFree(conn->env, OCI_HTYPE_ENV);
    free(conn);
}

int
ora_exec(OraConn *conn, const char *sql, char *errbuf, size_t errlen)
{
    OCIStmt *stmt = NULL;
    sword    rc;

    OCIHandleAlloc(conn->env, (void **) &stmt, OCI_HTYPE_STMT, 0, NULL);
    rc = OCIStmtPrepare(stmt, conn->err, (const OraText *) sql, (ub4) strlen(sql),
                        OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (OK(rc))
        rc = OCIStmtExecute(conn->svc, stmt, conn->err, 1, 0, NULL, NULL,
                            OCI_DEFAULT);
    if (!OK(rc))
        ora_geterr(conn, errbuf, errlen, sql);
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    return OK(rc) ? 0 : -1;
}

OraCursor *
ora_open(OraConn *conn, const char *sql, int ncols, const int *kinds,
         const unsigned *bufsz, char *errbuf, size_t errlen)
{
    OraCursor *cur = calloc(1, sizeof(OraCursor));
    ub4        prefetch = ORA_PREFETCH_ROWS;
    sword      rc;
    int        i;

    if (!cur)
    {
        snprintf(errbuf, errlen, "out of memory");
        return NULL;
    }
    cur->conn = conn;
    cur->ncols = ncols;
    cur->cols = calloc((size_t) ncols, sizeof(OraColBuf));
    OCIHandleAlloc(conn->env, (void **) &cur->stmt, OCI_HTYPE_STMT, 0, NULL);

    rc = OCIStmtPrepare(cur->stmt, conn->err, (const OraText *) sql,
                        (ub4) strlen(sql), OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (!OK(rc))
        goto fail;
    OCIAttrSet(cur->stmt, OCI_HTYPE_STMT, &prefetch, 0, OCI_ATTR_PREFETCH_ROWS,
               conn->err);

    for (i = 0; i < ncols; i++)
    {
        OCIDefine *def = NULL;

        cur->cols[i].bufsz = bufsz[i];
        cur->cols[i].buf = malloc(bufsz[i] + 1);
        rc = OCIDefineByPos(cur->stmt, &def, conn->err, (ub4) (i + 1),
                            cur->cols[i].buf, (sb4) bufsz[i],
                            kinds[i] == ORA_COL_BINARY ? SQLT_BIN : SQLT_CHR,
                            &cur->cols[i].ind, &cur->cols[i].len,
                            &cur->cols[i].rc, OCI_DEFAULT);
        if (!OK(rc))
            goto fail;
    }

    /* iters = 0: parse/execute only; rows are pulled with OCIStmtFetch2. */
    rc = OCIStmtExecute(conn->svc, cur->stmt, conn->err, 0, 0, NULL, NULL,
                        OCI_DEFAULT);
    if (!OK(rc))
        goto fail;
    return cur;

fail:
    ora_geterr(conn, errbuf, errlen, "Oracle query");
    ora_close(cur);
    return NULL;
}

int
ora_fetch(OraCursor *cur, char *errbuf, size_t errlen)
{
    sword rc = OCIStmtFetch2(cur->stmt, cur->conn->err, 1, OCI_FETCH_NEXT, 0,
                             OCI_DEFAULT);
    int   i;

    if (rc == OCI_NO_DATA)
        return 0;
    if (!OK(rc))
    {
        ora_geterr(cur->conn, errbuf, errlen, "Oracle fetch");
        return -1;
    }
    /* A value larger than its buffer comes back as rc 1406 (truncated). */
    for (i = 0; i < cur->ncols; i++)
    {
        if (cur->cols[i].ind != -1 &&
            (cur->cols[i].rc == 1406 || cur->cols[i].ind > 0))
        {
            snprintf(errbuf, errlen,
                     "value in column #%d exceeds the %u-byte fetch buffer "
                     "(LOB larger than the allowed maximum?)",
                     i + 1, cur->cols[i].bufsz);
            return -1;
        }
    }
    return 1;
}

const char *
ora_value(OraCursor *cur, int col, size_t *len)
{
    if (cur->cols[col].ind == -1)
        return NULL;
    if (len)
        *len = cur->cols[col].len;
    return cur->cols[col].buf;
}

void
ora_close(OraCursor *cur)
{
    int i;

    if (!cur)
        return;
    if (cur->stmt)
        OCIHandleFree(cur->stmt, OCI_HTYPE_STMT);
    for (i = 0; i < cur->ncols; i++)
        free(cur->cols[i].buf);
    free(cur->cols);
    free(cur);
}

#endif /* PGCLONE_WITH_ORACLE */
