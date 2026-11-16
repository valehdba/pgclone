/*
 * pgclone_tds.c - FreeTDS (DB-Library) access for pgclone.table_from_sqlserver().
 *
 * Deliberately does NOT include any PostgreSQL header (see pgclone_tds.h).
 * Compiled in full only with -DPGCLONE_WITH_SQLSERVER (make WITH_SQLSERVER=1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pgclone_tds.h"

#ifndef PGCLONE_WITH_SQLSERVER

/* ---------------- stub build: no FreeTDS available ---------------- */
static int
tds_unsupported(char *errbuf, size_t errlen)
{
    snprintf(errbuf, errlen,
             "pgclone was built without SQL Server support "
             "(rebuild with: make WITH_SQLSERVER=1 [FREETDS_HOME=/path/to/freetds])");
    return -1;
}

int tds_is_available(void) { return 0; }
TdsConn *tds_connect(const char *h, const char *p, const char *d, const char *u,
                     const char *w, int enc, char *e, size_t l)
{ (void) h; (void) p; (void) d; (void) u; (void) w; (void) enc; tds_unsupported(e, l); return NULL; }
void tds_disconnect(TdsConn *conn) { (void) conn; }
int tds_exec(TdsConn *conn, const char *sql, char *e, size_t l)
{ (void) conn; (void) sql; return tds_unsupported(e, l); }
TdsCursor *tds_open(TdsConn *conn, const char *sql, int n, char *e, size_t l)
{ (void) conn; (void) sql; (void) n; tds_unsupported(e, l); return NULL; }
int tds_fetch(TdsCursor *cur, char *e, size_t l)
{ (void) cur; return tds_unsupported(e, l); }
const char *tds_value(TdsCursor *cur, int col, size_t *len)
{ (void) cur; (void) col; if (len) *len = 0; return NULL; }
void tds_close(TdsCursor *cur) { (void) cur; }

#else /* PGCLONE_WITH_SQLSERVER */

#include <sybfront.h>
#include <sybdb.h>

#define TDS_LOGIN_TIMEOUT_SEC 15

struct TdsConn
{
    DBPROCESS  *dbproc;
};

struct TdsCursor
{
    TdsConn    *conn;
    int         ncols;
};

/*
 * DB-Library reports problems through process-global callbacks. The backend
 * is single-threaded, so one static buffer is enough: the handlers record the
 * most recent message and the caller copies it into its errbuf.
 */
static char tds_lasterr[1024];
static int  tds_initialized = 0;

static int
tds_err_handler(DBPROCESS *dbproc, int severity, int dberr, int oserr,
                char *dberrstr, char *oserrstr)
{
    (void) dbproc; (void) severity; (void) dberr;
    if (oserr != DBNOERR && oserrstr && oserrstr[0])
        snprintf(tds_lasterr, sizeof(tds_lasterr), "%s (%s)",
                 dberrstr ? dberrstr : "DB-Library error", oserrstr);
    else
        snprintf(tds_lasterr, sizeof(tds_lasterr), "%s",
                 dberrstr ? dberrstr : "DB-Library error");
    return INT_CANCEL;
}

static int
tds_msg_handler(DBPROCESS *dbproc, DBINT msgno, int msgstate, int severity,
                char *msgtext, char *srvname, char *procname, int line)
{
    (void) dbproc; (void) msgstate; (void) srvname; (void) procname; (void) line;
    /* Severity <= 10 is informational ("Changed database context to ..."). */
    if (severity > 10)
        snprintf(tds_lasterr, sizeof(tds_lasterr), "%s (SQL Server error %ld)",
                 msgtext ? msgtext : "SQL Server error", (long) msgno);
    return 0;
}

static void
tds_set_err(char *errbuf, size_t errlen, const char *what)
{
    snprintf(errbuf, errlen, "%s failed: %s", what,
             tds_lasterr[0] ? tds_lasterr : "unknown error");
}

int tds_is_available(void) { return 1; }

TdsConn *
tds_connect(const char *host, const char *port, const char *database,
            const char *user, const char *password, int encrypt,
            char *errbuf, size_t errlen)
{
    TdsConn    *conn;
    LOGINREC   *login;
    char        server[512];

    if (!tds_initialized)
    {
        if (dbinit() == FAIL)
        {
            snprintf(errbuf, errlen, "dbinit failed (is FreeTDS usable?)");
            return NULL;
        }
        dberrhandle(tds_err_handler);
        dbmsghandle(tds_msg_handler);
        dbsetlogintime(TDS_LOGIN_TIMEOUT_SEC);
        tds_initialized = 1;
    }
    tds_lasterr[0] = '\0';

    login = dblogin();
    if (!login)
    {
        snprintf(errbuf, errlen, "dblogin failed: out of memory");
        return NULL;
    }
    DBSETLUSER(login, user);
    DBSETLPWD(login, password);
    DBSETLAPP(login, "pgclone");
    DBSETLCHARSET(login, "UTF-8");      /* PostgreSQL side is UTF-8 */
#ifdef DBVERSION_74
    DBSETLVERSION(login, DBVERSION_74); /* TDS 7.4: SQL Server 2012+ */
#else
    DBSETLVERSION(login, DBVERSION_72);
#endif
#ifdef DBSETENCRYPT
    if (encrypt >= 0)
        DBSETLENCRYPT(login, encrypt ? TRUE : FALSE);
#else
    (void) encrypt;
#endif

    snprintf(server, sizeof(server), "%s:%s", host, port);

    conn = calloc(1, sizeof(TdsConn));
    if (!conn)
    {
        dbloginfree(login);
        snprintf(errbuf, errlen, "out of memory");
        return NULL;
    }
    conn->dbproc = dbopen(login, server);
    dbloginfree(login);
    if (!conn->dbproc)
    {
        tds_set_err(errbuf, errlen, "SQL Server login");
        free(conn);
        return NULL;
    }
    if (database && database[0] && dbuse(conn->dbproc, database) == FAIL)
    {
        tds_set_err(errbuf, errlen, "SQL Server USE database");
        tds_disconnect(conn);
        return NULL;
    }
    return conn;
}

void
tds_disconnect(TdsConn *conn)
{
    if (!conn)
        return;
    if (conn->dbproc)
        dbclose(conn->dbproc);
    free(conn);
}

/* Send a batch and drain all of its results. */
int
tds_exec(TdsConn *conn, const char *sql, char *errbuf, size_t errlen)
{
    RETCODE rc;

    tds_lasterr[0] = '\0';
    if (dbcmd(conn->dbproc, sql) == FAIL || dbsqlexec(conn->dbproc) == FAIL)
    {
        tds_set_err(errbuf, errlen, sql);
        dbcancel(conn->dbproc);
        return -1;
    }
    while ((rc = dbresults(conn->dbproc)) != NO_MORE_RESULTS)
    {
        if (rc == FAIL)
        {
            tds_set_err(errbuf, errlen, sql);
            dbcancel(conn->dbproc);
            return -1;
        }
        while (dbnextrow(conn->dbproc) != NO_MORE_ROWS)
            ;
    }
    return 0;
}

TdsCursor *
tds_open(TdsConn *conn, const char *sql, int ncols, char *errbuf, size_t errlen)
{
    TdsCursor  *cur;
    RETCODE     rc;

    tds_lasterr[0] = '\0';
    if (dbcmd(conn->dbproc, sql) == FAIL || dbsqlexec(conn->dbproc) == FAIL)
    {
        tds_set_err(errbuf, errlen, "SQL Server query");
        dbcancel(conn->dbproc);
        return NULL;
    }
    rc = dbresults(conn->dbproc);
    if (rc != SUCCEED)
    {
        tds_set_err(errbuf, errlen, "SQL Server query");
        dbcancel(conn->dbproc);
        return NULL;
    }
    if (dbnumcols(conn->dbproc) != ncols)
    {
        snprintf(errbuf, errlen,
                 "SQL Server query returned %d columns, expected %d",
                 dbnumcols(conn->dbproc), ncols);
        dbcancel(conn->dbproc);
        return NULL;
    }
    cur = calloc(1, sizeof(TdsCursor));
    if (!cur)
    {
        snprintf(errbuf, errlen, "out of memory");
        dbcancel(conn->dbproc);
        return NULL;
    }
    cur->conn = conn;
    cur->ncols = ncols;
    return cur;
}

int
tds_fetch(TdsCursor *cur, char *errbuf, size_t errlen)
{
    RETCODE rc;

    tds_lasterr[0] = '\0';
    rc = dbnextrow(cur->conn->dbproc);
    if (rc == REG_ROW)
        return 1;
    if (rc == NO_MORE_ROWS)
        return 0;
    tds_set_err(errbuf, errlen, "SQL Server fetch");
    return -1;
}

const char *
tds_value(TdsCursor *cur, int col, size_t *len)
{
    DBPROCESS  *p = cur->conn->dbproc;
    BYTE       *d = dbdata(p, col + 1);

    if (d == NULL)
    {
        if (len)
            *len = 0;
        return NULL;                /* SQL NULL */
    }
    if (len)
        *len = (size_t) dbdatlen(p, col + 1);
    return (const char *) d;
}

void
tds_close(TdsCursor *cur)
{
    if (!cur)
        return;
    /* Discard whatever is left of the result set so the connection is reusable. */
    dbcancel(cur->conn->dbproc);
    free(cur);
}

#endif /* PGCLONE_WITH_SQLSERVER */
