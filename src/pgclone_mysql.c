/*
 * pgclone_mysql.c - clone a table from a MySQL / MariaDB database into PostgreSQL.
 *
 *   pgclone.table_from_mysql(mysql_conninfo, mysql_database, mysql_table,
 *                            include_data, target_schema, target_table,
 *                            dry_run)
 *   pgclone.mysql_type_to_pg(data_type, column_type, char_length,
 *                            numeric_precision, numeric_scale,
 *                            datetime_precision)
 *
 * Flow: read column + PRIMARY KEY / UNIQUE metadata from
 * information_schema.columns / statistics, map MySQL types to PostgreSQL
 * types, then (on a loopback connection, inside one transaction) CREATE TABLE
 * and stream the rows with COPY ... FROM STDIN. MySQL access goes through
 * pgclone_myc.c, which is only functional when built with WITH_MYSQL=1.
 *
 * MySQL calls a database a "schema"; mysql_database is that database.
 * All values are read through the text protocol; binary columns are fetched
 * as HEX() and BIT columns as a zero-padded bit string.
 */
#include "postgres.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "libpq-fe.h"

#include "pgclone_myc.h"

extern PGconn *pgclone_connect_local(void);

#define MY_ERRLEN         1024
#define COPY_FLUSH_BYTES  (64 * 1024)

PG_FUNCTION_INFO_V1(pgclone_table_from_mysql);
PG_FUNCTION_INFO_V1(pgclone_mysql_type_to_pg);

/* ------------------------------------------------------------------ */
/* Type mapping                                                       */
/* ------------------------------------------------------------------ */
typedef enum MyKind
{
    MK_UNSUPPORTED = 0,
    MK_PLAIN,       /* integers, decimal, float, text, enum, set, json, year, time */
    MK_BIT,         /* bit(n)                                                      */
    MK_DATE,        /* date                                                        */
    MK_DATETIME,    /* datetime(n)                                                 */
    MK_TIMESTAMP,   /* timestamp(n): stored as UTC, read with time_zone '+00:00'   */
    MK_BINARY       /* binary / varbinary / *blob                                  */
} MyKind;

/* column_type may be NULL (only used to detect "unsigned"); other -1 = NULL. */
static const char *
map_mysql_type(const char *dt, const char *column_type, int char_length,
               int prec, int scale, int dt_prec, MyKind *kind)
{
    MyKind      k = MK_UNSUPPORTED;
    const char *r = NULL;
    bool        uns = (column_type != NULL && strstr(column_type, "unsigned") != NULL);

    if (strcmp(dt, "tinyint") == 0)
    {
        /* tinyint(1) is NOT mapped to boolean: values other than 0/1 are legal. */
        k = MK_PLAIN;
        r = "smallint";
    }
    else if (strcmp(dt, "smallint") == 0)
    {
        k = MK_PLAIN;
        r = uns ? "integer" : "smallint";
    }
    else if (strcmp(dt, "mediumint") == 0)
    {
        k = MK_PLAIN;
        r = "integer";
    }
    else if (strcmp(dt, "int") == 0 || strcmp(dt, "integer") == 0)
    {
        k = MK_PLAIN;
        r = uns ? "bigint" : "integer";
    }
    else if (strcmp(dt, "bigint") == 0)
    {
        k = MK_PLAIN;
        r = uns ? "numeric(20)" : "bigint";
    }
    else if (strcmp(dt, "decimal") == 0 || strcmp(dt, "numeric") == 0 ||
             strcmp(dt, "dec") == 0 || strcmp(dt, "fixed") == 0)
    {
        k = MK_PLAIN;
        if (prec < 0)
            r = "numeric";
        else if (scale > 0)
            r = psprintf("numeric(%d,%d)", Max(prec, scale), scale);
        else
            r = psprintf("numeric(%d)", prec);
    }
    else if (strcmp(dt, "float") == 0)
    {
        k = MK_PLAIN;
        r = "real";
    }
    else if (strcmp(dt, "double") == 0 || strcmp(dt, "real") == 0)
    {
        k = MK_PLAIN;
        r = "double precision";
    }
    else if (strcmp(dt, "bit") == 0)
    {
        k = MK_BIT;
        r = psprintf("bit(%d)", prec > 0 ? prec : 1);
    }
    else if (strcmp(dt, "char") == 0)
    {
        k = MK_PLAIN;
        r = char_length > 0 ? psprintf("char(%d)", char_length) : "char(1)";
    }
    else if (strcmp(dt, "varchar") == 0)
    {
        k = MK_PLAIN;
        r = char_length > 0 ? psprintf("varchar(%d)", char_length) : "text";
    }
    else if (strcmp(dt, "tinytext") == 0 || strcmp(dt, "text") == 0 ||
             strcmp(dt, "mediumtext") == 0 || strcmp(dt, "longtext") == 0 ||
             strcmp(dt, "enum") == 0 || strcmp(dt, "set") == 0)
    {
        k = MK_PLAIN;
        r = "text";
    }
    else if (strcmp(dt, "json") == 0)
    {
        k = MK_PLAIN;
        r = "jsonb";
    }
    else if (strcmp(dt, "year") == 0)
    {
        k = MK_PLAIN;
        r = "smallint";
    }
    else if (strcmp(dt, "date") == 0)
    {
        k = MK_DATE;
        r = "date";
    }
    else if (strcmp(dt, "time") == 0)
    {
        /* MySQL TIME spans -838:59:59 .. 838:59:59, which PostgreSQL time cannot hold. */
        k = MK_PLAIN;
        r = psprintf("interval(%d)", dt_prec >= 0 ? Min(dt_prec, 6) : 0);
    }
    else if (strcmp(dt, "datetime") == 0)
    {
        k = MK_DATETIME;
        r = psprintf("timestamp(%d)", dt_prec >= 0 ? Min(dt_prec, 6) : 0);
    }
    else if (strcmp(dt, "timestamp") == 0)
    {
        k = MK_TIMESTAMP;
        r = psprintf("timestamptz(%d)", dt_prec >= 0 ? Min(dt_prec, 6) : 0);
    }
    else if (strcmp(dt, "binary") == 0 || strcmp(dt, "varbinary") == 0 ||
             strcmp(dt, "tinyblob") == 0 || strcmp(dt, "blob") == 0 ||
             strcmp(dt, "mediumblob") == 0 || strcmp(dt, "longblob") == 0)
    {
        k = MK_BINARY;
        r = "bytea";
    }
    /* Everything else (geometry, point, linestring, polygon, multi*,
     * geometrycollection, vector) is not supported yet. */

    if (kind)
        *kind = k;
    return r;
}

/*
 * SQL-callable wrapper so the mapping is testable without a MySQL client.
 * The arguments are the columns of information_schema.columns of the same
 * names (column_type is only used to detect "unsigned"). For BIT pass the
 * width as numeric_precision; for TIME/DATETIME/TIMESTAMP pass the
 * fractional-seconds precision as datetime_precision.
 */
Datum
pgclone_mysql_type_to_pg(PG_FUNCTION_ARGS)
{
    char       *dt;
    char       *ct = NULL;
    const char *r;
    char       *p;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();
    dt = text_to_cstring(PG_GETARG_TEXT_PP(0));
    for (p = dt; *p; p++)
        *p = pg_tolower((unsigned char) *p);

    if (!PG_ARGISNULL(1))
    {
        ct = text_to_cstring(PG_GETARG_TEXT_PP(1));
        for (p = ct; *p; p++)
            *p = pg_tolower((unsigned char) *p);
    }

    r = map_mysql_type(dt, ct,
                       PG_ARGISNULL(2) ? -1 : PG_GETARG_INT32(2),
                       PG_ARGISNULL(3) ? -1 : PG_GETARG_INT32(3),
                       PG_ARGISNULL(4) ? -1 : PG_GETARG_INT32(4),
                       PG_ARGISNULL(5) ? -1 : PG_GETARG_INT32(5),
                       NULL);
    if (r == NULL)
        PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(r));
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */
typedef struct MyColumn
{
    char       *my_name;
    char       *my_type;
    char       *pg_name;
    const char *pg_type;
    MyKind      kind;
    bool        notnull;
    bool        skip;        /* unsupported type */
    char       *select_expr;
} MyColumn;

/* text -> 'te''xt' (MySQL string literal; sql_mode is reset so backslash
 * escapes are in force, hence both ' and \ are doubled). */
static char *
my_literal(const char *s)
{
    StringInfoData b;

    initStringInfo(&b);
    appendStringInfoChar(&b, '\'');
    for (; *s; s++)
    {
        if (*s == '\'' || *s == '\\')
            appendStringInfoChar(&b, *s);
        appendStringInfoChar(&b, *s);
    }
    appendStringInfoChar(&b, '\'');
    return b.data;
}

/* name -> `name` (MySQL quoted identifier) */
static char *
my_ident(const char *s)
{
    StringInfoData b;

    initStringInfo(&b);
    appendStringInfoChar(&b, '`');
    for (; *s; s++)
    {
        if (*s == '`')
            appendStringInfoChar(&b, '`');
        appendStringInfoChar(&b, *s);
    }
    appendStringInfoChar(&b, '`');
    return b.data;
}

static char *
my_lower_ascii(const char *s)
{
    char *r = pstrdup(s);
    char *p;

    for (p = r; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
    return r;
}

/* Parse "key=value key='quoted value' ..." (libpq style). */
typedef struct MyConnSpec
{
    char *host, *port, *user, *password;
    int   ssl;               /* -1 = client default */
} MyConnSpec;

static void
parse_mysql_conninfo(const char *ci, MyConnSpec *o)
{
    const char *p = ci;

    memset(o, 0, sizeof(*o));
    o->ssl = -1;
    while (*p)
    {
        StringInfoData key, val;

        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        if (!*p)
            break;
        initStringInfo(&key);
        initStringInfo(&val);
        while (*p && *p != '=' && *p != ' ')
            appendStringInfoChar(&key, *p++);
        if (*p != '=')
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("pgclone: invalid MySQL conninfo near \"%s\"", key.data),
                     errhint("Use: host=H port=3306 user=U password=P")));
        p++;
        if (*p == '\'')
        {
            p++;
            while (*p && *p != '\'')
            {
                if (*p == '\\' && p[1])
                    p++;
                appendStringInfoChar(&val, *p++);
            }
            if (*p == '\'')
                p++;
        }
        else
            while (*p && *p != ' ' && *p != '\t' && *p != '\n')
                appendStringInfoChar(&val, *p++);

        if (strcmp(key.data, "host") == 0)
            o->host = val.data;
        else if (strcmp(key.data, "port") == 0)
            o->port = val.data;
        else if (strcmp(key.data, "user") == 0)
            o->user = val.data;
        else if (strcmp(key.data, "password") == 0)
            o->password = val.data;
        else if (strcmp(key.data, "ssl") == 0)
        {
            if (strcmp(val.data, "yes") == 0 || strcmp(val.data, "true") == 0 ||
                strcmp(val.data, "on") == 0 || strcmp(val.data, "require") == 0)
                o->ssl = 1;
            else if (strcmp(val.data, "no") == 0 || strcmp(val.data, "false") == 0 ||
                     strcmp(val.data, "off") == 0)
                o->ssl = 0;
            else
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("pgclone: invalid MySQL conninfo value for ssl: \"%s\"", val.data),
                         errhint("Use ssl=yes or ssl=no.")));
        }
        else
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("pgclone: unknown MySQL conninfo key \"%s\"", key.data)));
    }

    if (o->password == NULL)
    {
        /* Keep secrets out of SQL text / logs when possible. */
        const char *env = getenv("PGCLONE_MYSQL_PASSWORD");

        if (env)
            o->password = pstrdup(env);
    }
    if (!o->user || !o->password)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone: MySQL conninfo needs user and password"),
                 errhint("password may instead come from the PGCLONE_MYSQL_PASSWORD environment variable of the server process.")));
    if (!o->host)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone: MySQL conninfo needs host")));
    if (!o->port)
        o->port = "3306";
}

/* Append s to a COPY text-format buffer, escaping as required. */
static void
copy_escape(StringInfo b, const char *s, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        switch (s[i])
        {
            case '\\': appendStringInfoString(b, "\\\\"); break;
            case '\n': appendStringInfoString(b, "\\n");  break;
            case '\r': appendStringInfoString(b, "\\r");  break;
            case '\t': appendStringInfoString(b, "\\t");  break;
            case '\0': break;   /* PostgreSQL text cannot hold NUL */
            default:   appendStringInfoChar(b, s[i]);
        }
    }
}

/*
 * MySQL can store "zero" and partial-zero dates ('0000-00-00',
 * '2024-00-15 ...') that PostgreSQL rejects. Such values are loaded as NULL.
 * v is 'YYYY-MM-DD[ ...]'.
 */
static bool
my_zero_date(const char *v, size_t len)
{
    if (len < 10)
        return false;
    return (v[0] == '0' && v[1] == '0' && v[2] == '0' && v[3] == '0') ||
           (v[5] == '0' && v[6] == '0') ||
           (v[8] == '0' && v[9] == '0');
}

#define MY_FAIL(buf) \
    ereport(ERROR, \
            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), \
             errmsg("pgclone: MySQL: %s", (buf))))

/* ------------------------------------------------------------------ */
/* Metadata                                                           */
/* ------------------------------------------------------------------ */
static int
fetch_columns(MycConn *conn, const char *db, const char *table, MyColumn **out)
{
    char        err[MY_ERRLEN];
    StringInfoData q;
    MycCursor  *cur;
    MyColumn   *cols = NULL;
    int         n = 0, cap = 0, rc;

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT column_name, data_type, column_type, character_maximum_length, "
        "numeric_precision, numeric_scale, datetime_precision, is_nullable "
        "FROM information_schema.columns "
        "WHERE table_schema = %s AND table_name = %s "
        "ORDER BY ordinal_position",
        my_literal(db), my_literal(table));

    cur = myc_open(conn, q.data, 8, err, sizeof(err));
    if (!cur)
        MY_FAIL(err);

    while ((rc = myc_fetch(cur, err, sizeof(err))) == 1)
    {
        MyColumn   *c;
        const char *v[8];
        int         i;
        size_t      l;
        int         clen;

        if (n == cap)
        {
            cap = cap ? cap * 2 : 32;
            cols = cols ? repalloc(cols, cap * sizeof(MyColumn))
                        : palloc(cap * sizeof(MyColumn));
        }
        for (i = 0; i < 8; i++)
        {
            const char *raw = myc_value(cur, i, &l);

            v[i] = raw ? pnstrdup(raw, l) : NULL;
        }
        c = &cols[n++];
        memset(c, 0, sizeof(*c));
        c->my_name = (char *) v[0];
        c->my_type = v[1] ? my_lower_ascii(v[1]) : "";
        c->notnull = (v[7] && v[7][0] == 'N');
        /* character_maximum_length is a byte limit for the *text types,
         * which map to text anyway; only char/varchar use it. */
        clen = v[3] ? (int) Min(strtoll(v[3], NULL, 10), 10485760) : -1;
        c->pg_type = map_mysql_type(c->my_type,
                                    v[2] ? my_lower_ascii(v[2]) : NULL,
                                    clen,
                                    v[4] ? atoi(v[4]) : -1,
                                    v[5] ? atoi(v[5]) : -1,
                                    v[6] ? atoi(v[6]) : -1,
                                    &c->kind);
        c->skip = (c->pg_type == NULL);
        c->pg_name = my_lower_ascii(c->my_name);

        if (!c->skip)
        {
            char *id = my_ident(c->my_name);

            switch (c->kind)
            {
                case MK_BIT:
                {
                    int w = v[4] ? atoi(v[4]) : 1;

                    c->select_expr = psprintf("LPAD(BIN(%s + 0), %d, '0')", id, w > 0 ? w : 1);
                    break;
                }
                case MK_BINARY:
                    c->select_expr = psprintf("HEX(%s)", id);
                    break;
                case MK_PLAIN:
                case MK_DATE:
                case MK_DATETIME:
                case MK_TIMESTAMP:
                    c->select_expr = id;
                    break;
                default:
                    c->skip = true;
            }
        }
    }
    if (rc < 0)
    {
        myc_close(cur);
        MY_FAIL(err);
    }
    myc_close(cur);
    *out = cols;
    return n;
}

/*
 * Build ", PRIMARY KEY (...), UNIQUE (...)" clauses. Constraints touching a
 * skipped column, a column prefix (UNIQUE (c(10))) or an expression are
 * dropped with a NOTICE.
 */
static void
append_constraints(MycConn *conn, const char *db, const char *table,
                   MyColumn *cols, int ncols, StringInfo ddl)
{
    char        err[MY_ERRLEN];
    StringInfoData q, cur_cols;
    MycCursor  *cur;
    char       *cur_name = NULL;
    bool        cur_ok = true;
    int         rc;

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT index_name, column_name, sub_part "
        "FROM information_schema.statistics "
        "WHERE table_schema = %s AND table_name = %s AND non_unique = 0 "
        "ORDER BY (index_name = 'PRIMARY') DESC, index_name, seq_in_index",
        my_literal(db), my_literal(table));

    cur = myc_open(conn, q.data, 3, err, sizeof(err));
    if (!cur)
        MY_FAIL(err);

    initStringInfo(&cur_cols);

#define FLUSH_CONSTRAINT() \
    do { \
        if (cur_name) \
        { \
            if (cur_ok) \
                appendStringInfo(ddl, ", %s (%s)", \
                                 strcmp(cur_name, "PRIMARY") == 0 ? "PRIMARY KEY" : "UNIQUE", \
                                 cur_cols.data); \
            else \
                ereport(NOTICE, (errmsg("pgclone: skipping MySQL index %s " \
                                        "(involves an unsupported column, a column prefix or an expression)", \
                                        cur_name))); \
        } \
    } while (0)

    while ((rc = myc_fetch(cur, err, sizeof(err))) == 1)
    {
        size_t      l;
        const char *nm = pnstrdup(myc_value(cur, 0, &l), l);
        const char *cn_raw = myc_value(cur, 1, &l);
        const char *cn = cn_raw ? pnstrdup(cn_raw, l) : NULL;
        bool        has_prefix = (myc_value(cur, 2, &l) != NULL);
        int         i;
        MyColumn   *found = NULL;

        if (cur_name == NULL || strcmp(cur_name, nm) != 0)
        {
            FLUSH_CONSTRAINT();
            cur_name = (char *) nm;
            cur_ok = true;
            resetStringInfo(&cur_cols);
        }
        if (cn != NULL)
            for (i = 0; i < ncols; i++)
                if (strcmp(cols[i].my_name, cn) == 0)
                    found = &cols[i];
        if (!found || found->skip || has_prefix)
            cur_ok = false;
        else
        {
            if (cur_cols.len > 0)
                appendStringInfoString(&cur_cols, ", ");
            appendStringInfoString(&cur_cols, quote_identifier(found->pg_name));
        }
    }
    if (rc < 0)
    {
        myc_close(cur);
        MY_FAIL(err);
    }
    FLUSH_CONSTRAINT();
    myc_close(cur);
#undef FLUSH_CONSTRAINT
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                   */
/* ------------------------------------------------------------------ */
Datum
pgclone_table_from_mysql(PG_FUNCTION_ARGS)
{
    char       *conninfo, *my_db, *my_table;
    bool        include_data, dry_run;
    char       *tgt_schema, *tgt_table;
    MyConnSpec  spec;
    char        err[MY_ERRLEN];
    MycConn *volatile mconn = NULL;
    MycCursor *volatile cursor = NULL;
    PGconn   *volatile lconn = NULL;
    volatile bool in_txn = false;
    MyColumn   *cols = NULL;
    int         ncols, i, nsel = 0;
    StringInfoData ddl, sel, cmd;
    int64       rows = 0, zero_dates = 0;
    char       *result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("pgclone: mysql_conninfo, mysql_database and mysql_table are required")));

    conninfo     = text_to_cstring(PG_GETARG_TEXT_PP(0));
    my_db        = text_to_cstring(PG_GETARG_TEXT_PP(1));
    my_table     = text_to_cstring(PG_GETARG_TEXT_PP(2));
    include_data = PG_ARGISNULL(3) ? true : PG_GETARG_BOOL(3);
    tgt_schema   = PG_ARGISNULL(4) ? "public" : text_to_cstring(PG_GETARG_TEXT_PP(4));
    tgt_table    = PG_ARGISNULL(5) ? my_lower_ascii(my_table)
                                   : text_to_cstring(PG_GETARG_TEXT_PP(5));
    dry_run      = PG_ARGISNULL(6) ? false : PG_GETARG_BOOL(6);

    if (!myc_is_available())
    {
        myc_connect("", "", "", "", -1, err, sizeof(err));   /* fills the stub message */
        MY_FAIL(err);
    }

    parse_mysql_conninfo(conninfo, &spec);

    PG_TRY();
    {
        mconn = myc_connect(spec.host, spec.port, spec.user, spec.password,
                            spec.ssl, err, sizeof(err));
        if (!mconn)
            MY_FAIL(err);

        /*
         * Deterministic text formats: UTC for TIMESTAMP columns, no sql_mode
         * surprises (backslash escapes on, no zero-date errors on read), and
         * one consistent InnoDB snapshot.
         */
        if (myc_exec(mconn, "SET SESSION time_zone = '+00:00'", err, sizeof(err)) < 0 ||
            myc_exec(mconn, "SET SESSION sql_mode = ''", err, sizeof(err)) < 0 ||
            myc_exec(mconn, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ",
                     err, sizeof(err)) < 0 ||
            myc_exec(mconn, "START TRANSACTION WITH CONSISTENT SNAPSHOT",
                     err, sizeof(err)) < 0)
            MY_FAIL(err);

        ncols = fetch_columns(mconn, my_db, my_table, &cols);
        if (ncols == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg("pgclone: MySQL table %s.%s not found or not visible to this user",
                            my_db, my_table)));

        /* Duplicate PG names (e.g. "Id" and "ID" in a case-sensitive schema) would collide. */
        for (i = 0; i < ncols; i++)
        {
            int j;

            for (j = 0; j < i; j++)
                if (!cols[i].skip && !cols[j].skip &&
                    strcmp(cols[i].pg_name, cols[j].pg_name) == 0)
                    ereport(ERROR,
                            (errcode(ERRCODE_DUPLICATE_COLUMN),
                             errmsg("pgclone: MySQL columns \"%s\" and \"%s\" both map to PostgreSQL column \"%s\"",
                                    cols[j].my_name, cols[i].my_name, cols[i].pg_name)));
            if (cols[i].skip)
                ereport(WARNING,
                        (errmsg("pgclone: skipping column %s: MySQL type %s is not supported",
                                cols[i].my_name, cols[i].my_type)));
            else
                nsel++;
        }
        if (nsel == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("pgclone: no column of %s.%s has a supported type", my_db, my_table)));

        /* ---- DDL ---- */
        initStringInfo(&ddl);
        appendStringInfo(&ddl, "CREATE TABLE %s.%s (",
                         quote_identifier(tgt_schema), quote_identifier(tgt_table));
        {
            bool first = true;

            for (i = 0; i < ncols; i++)
            {
                if (cols[i].skip)
                    continue;
                appendStringInfo(&ddl, "%s%s %s%s", first ? "" : ", ",
                                 quote_identifier(cols[i].pg_name), cols[i].pg_type,
                                 cols[i].notnull ? " NOT NULL" : "");
                first = false;
            }
        }
        append_constraints(mconn, my_db, my_table, cols, ncols, &ddl);
        appendStringInfoChar(&ddl, ')');

        if (dry_run)
        {
            result = psprintf("-- dry run, nothing executed\n%s;", ddl.data);
        }
        else
        {
            PGresult *res;
            int       cret;
            int       k = 0;

            lconn = pgclone_connect_local();
            res = PQexec(lconn, "BEGIN");
            PQclear(res);
            in_txn = true;

            res = PQexec(lconn, ddl.data);
            if (PQresultStatus(res) != PGRES_COMMAND_OK)
            {
                char *m = pstrdup(PQerrorMessage(lconn));

                PQclear(res);
                ereport(ERROR,
                        (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                         errmsg("pgclone: could not create target table: %s", m)));
            }
            PQclear(res);

            if (include_data)
            {
                initStringInfo(&sel);
                initStringInfo(&cmd);
                appendStringInfo(&cmd, "COPY %s.%s (",
                                 quote_identifier(tgt_schema), quote_identifier(tgt_table));
                appendStringInfoString(&sel, "SELECT ");
                for (i = 0; i < ncols; i++)
                {
                    if (cols[i].skip)
                        continue;
                    appendStringInfo(&sel, "%s%s", k ? ", " : "", cols[i].select_expr);
                    appendStringInfo(&cmd, "%s%s", k ? ", " : "",
                                     quote_identifier(cols[i].pg_name));
                    k++;
                }
                appendStringInfo(&sel, " FROM %s.%s", my_ident(my_db), my_ident(my_table));
                appendStringInfoString(&cmd, ") FROM STDIN WITH (FORMAT text)");

                cursor = myc_open(mconn, sel.data, nsel, err, sizeof(err));
                if (!cursor)
                    MY_FAIL(err);

                res = PQexec(lconn, cmd.data);
                if (PQresultStatus(res) != PGRES_COPY_IN)
                {
                    char *m = pstrdup(PQerrorMessage(lconn));

                    PQclear(res);
                    ereport(ERROR,
                            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                             errmsg("pgclone: COPY IN failed on local: %s", m)));
                }
                PQclear(res);

                {
                    StringInfoData row;
                    int            fr;

                    initStringInfo(&row);
                    while ((fr = myc_fetch(cursor, err, sizeof(err))) == 1)
                    {
                        for (i = 0, k = 0; i < ncols; i++)
                        {
                            size_t      vl = 0;
                            const char *v;

                            if (cols[i].skip)
                                continue;
                            if (k > 0)
                                appendStringInfoChar(&row, '\t');
                            v = myc_value(cursor, k, &vl);
                            if (v == NULL)
                                appendStringInfoString(&row, "\\N");
                            else if (cols[i].kind == MK_BINARY)
                            {
                                /* already hex digits: \\x after COPY unescaping = \x */
                                appendStringInfoString(&row, "\\\\x");
                                appendBinaryStringInfo(&row, v, (int) vl);
                            }
                            else if ((cols[i].kind == MK_DATE ||
                                      cols[i].kind == MK_DATETIME ||
                                      cols[i].kind == MK_TIMESTAMP) &&
                                     my_zero_date(v, vl))
                            {
                                appendStringInfoString(&row, "\\N");
                                zero_dates++;
                            }
                            else
                            {
                                copy_escape(&row, v, vl);
                                if (cols[i].kind == MK_TIMESTAMP)
                                    appendStringInfoString(&row, "+00");   /* session time_zone is UTC */
                            }
                            k++;
                        }
                        appendStringInfoChar(&row, '\n');
                        rows++;

                        if (row.len >= COPY_FLUSH_BYTES)
                        {
                            if (PQputCopyData(lconn, row.data, row.len) != 1)
                                ereport(ERROR,
                                        (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                                         errmsg("pgclone: error writing COPY data to local: %s",
                                                PQerrorMessage(lconn))));
                            resetStringInfo(&row);
                            CHECK_FOR_INTERRUPTS();
                        }
                    }
                    if (fr < 0)
                    {
                        PQputCopyEnd(lconn, "mysql fetch error");
                        MY_FAIL(err);
                    }
                    if (row.len > 0 &&
                        PQputCopyData(lconn, row.data, row.len) != 1)
                        ereport(ERROR,
                                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                                 errmsg("pgclone: error writing COPY data to local: %s",
                                        PQerrorMessage(lconn))));
                }

                cret = PQputCopyEnd(lconn, NULL);
                if (cret != 1)
                    ereport(ERROR,
                            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                             errmsg("pgclone: COPY end failed: %s", PQerrorMessage(lconn))));
                res = PQgetResult(lconn);
                if (PQresultStatus(res) != PGRES_COMMAND_OK)
                {
                    char *m = pstrdup(PQerrorMessage(lconn));

                    PQclear(res);
                    ereport(ERROR,
                            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                             errmsg("pgclone: COPY into %s.%s failed: %s",
                                    tgt_schema, tgt_table, m)));
                }
                PQclear(res);
                while ((res = PQgetResult(lconn)) != NULL)
                    PQclear(res);
            }

            res = PQexec(lconn, "COMMIT");
            PQclear(res);
            in_txn = false;

            if (zero_dates > 0)
                ereport(NOTICE,
                        (errmsg("pgclone: %lld MySQL zero/partial-zero date value(s) loaded as NULL",
                                (long long) zero_dates)));
            result = psprintf("Cloned MySQL %s.%s -> %s.%s (%d columns, %lld rows)",
                              my_db, my_table, tgt_schema, tgt_table,
                              nsel, (long long) rows);
        }
    }
    PG_CATCH();
    {
        /*
         * Disconnect before closing the cursor: closing a streaming result
         * would first read (and discard) every row still pending.
         */
        if (mconn)
            myc_disconnect(mconn);
        if (cursor)
            myc_close(cursor);
        if (lconn)
        {
            if (in_txn)
            {
                PGresult *r = PQexec(lconn, "ROLLBACK");

                PQclear(r);
            }
            PQfinish(lconn);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (cursor)
        myc_close(cursor);
    if (mconn)
        myc_disconnect(mconn);
    if (lconn)
        PQfinish(lconn);

    PG_RETURN_TEXT_P(cstring_to_text(result));
}
