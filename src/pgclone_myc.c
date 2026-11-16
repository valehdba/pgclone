/*
 * pgclone_myc.c - MySQL client access for pgclone.table_from_mysql().
 *
 * Deliberately does NOT include any PostgreSQL header (see pgclone_myc.h).
 * Compiled in full only with -DPGCLONE_WITH_MYSQL (make WITH_MYSQL=1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pgclone_myc.h"

#ifndef PGCLONE_WITH_MYSQL

/* ---------------- stub build: no MySQL client library ---------------- */
static int
myc_unsupported(char *errbuf, size_t errlen)
{
    snprintf(errbuf, errlen,
             "pgclone was built without MySQL support "
             "(rebuild with: make WITH_MYSQL=1 [MYSQL_CONFIG=/path/to/mysql_config])");
    return -1;
}

int myc_is_available(void) { return 0; }
MycConn *myc_connect(const char *h, const char *p, const char *u, const char *w,
                     int ssl, char *e, size_t l)
{ (void) h; (void) p; (void) u; (void) w; (void) ssl; myc_unsupported(e, l); return NULL; }
void myc_disconnect(MycConn *conn) { (void) conn; }
int myc_exec(MycConn *conn, const char *sql, char *e, size_t l)
{ (void) conn; (void) sql; return myc_unsupported(e, l); }
MycCursor *myc_open(MycConn *conn, const char *sql, int n, char *e, size_t l)
{ (void) conn; (void) sql; (void) n; myc_unsupported(e, l); return NULL; }
int myc_fetch(MycCursor *cur, char *e, size_t l)
{ (void) cur; return myc_unsupported(e, l); }
const char *myc_value(MycCursor *cur, int col, size_t *len)
{ (void) cur; (void) col; if (len) *len = 0; return NULL; }
void myc_close(MycCursor *cur) { (void) cur; }

#else /* PGCLONE_WITH_MYSQL */

#include <mysql.h>

#define MYC_CONNECT_TIMEOUT_SEC 15

struct MycCursor;

struct MycConn
{
    MYSQL            m;         /* embedded: stays valid after mysql_close() */
    int              inited;
    int              connected;
    struct MycCursor *active;   /* open streaming result, if any */
};

struct MycCursor
{
    MycConn    *conn;
    MYSQL_RES  *res;
    MYSQL_ROW   row;
    unsigned long *lens;
    int         ncols;
};

int myc_is_available(void) { return 1; }

static void
myc_geterr(MycConn *conn, char *errbuf, size_t errlen, const char *what)
{
    snprintf(errbuf, errlen, "%s failed: %s (error %u)", what,
             mysql_error(&conn->m), mysql_errno(&conn->m));
}

MycConn *
myc_connect(const char *host, const char *port, const char *user,
            const char *password, int ssl, char *errbuf, size_t errlen)
{
    MycConn        *conn = calloc(1, sizeof(MycConn));
    unsigned int    tcp = MYSQL_PROTOCOL_TCP;
    unsigned int    timeout = MYC_CONNECT_TIMEOUT_SEC;

    if (!conn)
    {
        snprintf(errbuf, errlen, "out of memory");
        return NULL;
    }
    if (!mysql_init(&conn->m))
    {
        snprintf(errbuf, errlen, "mysql_init failed: out of memory");
        free(conn);
        return NULL;
    }
    conn->inited = 1;

    mysql_options(&conn->m, MYSQL_OPT_PROTOCOL, &tcp);    /* "localhost" must not mean a unix socket */
    mysql_options(&conn->m, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(&conn->m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
#if defined(MARIADB_PACKAGE_VERSION)
    if (ssl >= 0)
    {
        char enforce = ssl ? 1 : 0;

        mysql_options(&conn->m, MYSQL_OPT_SSL_ENFORCE, &enforce);
    }
#else
    if (ssl >= 0)
    {
        unsigned int mode = ssl ? SSL_MODE_REQUIRED : SSL_MODE_DISABLED;

        mysql_options(&conn->m, MYSQL_OPT_SSL_MODE, &mode);
    }
#endif

    if (!mysql_real_connect(&conn->m, host, user, password, NULL,
                            (unsigned int) atoi(port), NULL, 0))
    {
        myc_geterr(conn, errbuf, errlen, "MySQL login");
        myc_disconnect(conn);
        return NULL;
    }
    conn->connected = 1;
    return conn;
}

void
myc_disconnect(MycConn *conn)
{
    if (!conn)
        return;
    if (conn->inited)
        mysql_close(&conn->m);          /* drops the connection without draining a streaming result */
    if (conn->active)
    {
        /*
         * The connection is gone, so freeing the result must not try to read
         * the rest of it from the socket. m is embedded (not heap-allocated)
         * and zeroed by mysql_close(), so the result's handle pointer is still
         * valid and reports "no result pending".
         */
        if (conn->active->res)
            mysql_free_result(conn->active->res);
        conn->active->res = NULL;
        conn->active->conn = NULL;
    }
    free(conn);
}

int
myc_exec(MycConn *conn, const char *sql, char *errbuf, size_t errlen)
{
    if (mysql_query(&conn->m, sql) != 0)
    {
        myc_geterr(conn, errbuf, errlen, sql);
        return -1;
    }
    return 0;
}

MycCursor *
myc_open(MycConn *conn, const char *sql, int ncols, char *errbuf, size_t errlen)
{
    MycCursor *cur;

    if (mysql_real_query(&conn->m, sql, (unsigned long) strlen(sql)) != 0)
    {
        myc_geterr(conn, errbuf, errlen, "MySQL query");
        return NULL;
    }
    cur = calloc(1, sizeof(MycCursor));
    if (!cur)
    {
        snprintf(errbuf, errlen, "out of memory");
        return NULL;
    }
    cur->res = mysql_use_result(&conn->m);      /* streaming */
    if (!cur->res)
    {
        myc_geterr(conn, errbuf, errlen, "MySQL query");
        free(cur);
        return NULL;
    }
    if ((int) mysql_num_fields(cur->res) != ncols)
    {
        snprintf(errbuf, errlen, "MySQL query returned %d columns, expected %d",
                 (int) mysql_num_fields(cur->res), ncols);
        mysql_free_result(cur->res);
        free(cur);
        return NULL;
    }
    cur->conn = conn;
    cur->ncols = ncols;
    conn->active = cur;
    return cur;
}

int
myc_fetch(MycCursor *cur, char *errbuf, size_t errlen)
{
    if (!cur->res)
    {
        snprintf(errbuf, errlen, "MySQL fetch failed: connection closed");
        return -1;
    }
    cur->row = mysql_fetch_row(cur->res);
    if (cur->row)
    {
        cur->lens = mysql_fetch_lengths(cur->res);
        return 1;
    }
    if (mysql_errno(&cur->conn->m) != 0)
    {
        myc_geterr(cur->conn, errbuf, errlen, "MySQL fetch");
        return -1;
    }
    return 0;
}

const char *
myc_value(MycCursor *cur, int col, size_t *len)
{
    if (cur->row == NULL || cur->row[col] == NULL)
    {
        if (len)
            *len = 0;
        return NULL;                /* SQL NULL */
    }
    if (len)
        *len = (size_t) cur->lens[col];
    return cur->row[col];
}

void
myc_close(MycCursor *cur)
{
    if (!cur)
        return;
    if (cur->conn)
    {
        /* Reads and discards any rows still pending, so the connection stays usable. */
        if (cur->res)
            mysql_free_result(cur->res);
        cur->conn->active = NULL;
    }
    free(cur);
}

#endif /* PGCLONE_WITH_MYSQL */
