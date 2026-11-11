/*
 * pgclone_oracle.c - clone a table from an Oracle database into PostgreSQL.
 *
 *   pgclone.table_from_oracle(oracle_conninfo, oracle_schema, oracle_table,
 *                             include_data, target_schema, target_table,
 *                             dry_run)
 *   pgclone.oracle_type_to_pg(data_type, data_length, data_precision,
 *                             data_scale, char_length)
 *
 * Flow: read column + PK/UNIQUE metadata from ALL_TAB_COLUMNS /
 * ALL_CONSTRAINTS, map Oracle types to PostgreSQL types, then (on a loopback
 * connection, inside one transaction) CREATE TABLE and stream the rows with
 * COPY ... FROM STDIN. Oracle access goes through pgclone_oci.c, which is
 * only functional when built with WITH_ORACLE=1.
 */
#include "postgres.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "libpq-fe.h"

#include "pgclone_oci.h"

extern PGconn *pgclone_connect_local(void);

#define ORA_ERRLEN        1024
#define ORA_LOB_MAX_BYTES (4 * 1024 * 1024)   /* per-value cap for CLOB/NCLOB */
#define COPY_FLUSH_BYTES  (64 * 1024)

PG_FUNCTION_INFO_V1(pgclone_table_from_oracle);
PG_FUNCTION_INFO_V1(pgclone_oracle_type_to_pg);

/* ------------------------------------------------------------------ */
/* Type mapping                                                       */
/* ------------------------------------------------------------------ */
typedef enum OraKind
{
    OK_UNSUPPORTED = 0,
    OK_TEXT,        /* VARCHAR2 / CHAR / NVARCHAR2 / NCHAR      */
    OK_CLOB,        /* CLOB / NCLOB                              */
    OK_NUMBER,      /* NUMBER / FLOAT                            */
    OK_BINFLOAT,    /* BINARY_FLOAT / BINARY_DOUBLE              */
    OK_DATE,        /* DATE (carries a time part in Oracle)      */
    OK_TS,          /* TIMESTAMP(n)                              */
    OK_TSTZ,        /* TIMESTAMP(n) WITH TIME ZONE               */
    OK_TSLTZ,       /* TIMESTAMP(n) WITH LOCAL TIME ZONE         */
    OK_RAW,         /* RAW                                       */
    OK_ROWID        /* ROWID                                     */
} OraKind;

/* prec / scale / char_length: -1 means NULL in the Oracle catalog. */
static const char *
map_oracle_type(const char *dt, int data_length, int prec, int scale,
                int char_length, OraKind *kind)
{
    OraKind k = OK_UNSUPPORTED;
    const char *r = NULL;

    if (strcmp(dt, "NUMBER") == 0)
    {
        k = OK_NUMBER;
        if (prec < 0)
            r = "numeric";
        else if (scale == 0)
            r = (prec <= 4) ? "smallint" : (prec <= 9) ? "integer" :
                (prec <= 18) ? "bigint" : psprintf("numeric(%d)", prec);
        else if (scale > 0)
            /* Oracle allows scale > precision; PG14 does not. */
            r = psprintf("numeric(%d,%d)", Max(prec, scale), scale);
        else
            r = psprintf("numeric(%d)", Min(prec - scale, 1000));
    }
    else if (strcmp(dt, "FLOAT") == 0)
    {
        k = OK_NUMBER;
        r = (prec >= 0 && prec <= 24) ? "real" : "double precision";
    }
    else if (strcmp(dt, "BINARY_FLOAT") == 0)
    {
        k = OK_BINFLOAT;
        r = "real";
    }
    else if (strcmp(dt, "BINARY_DOUBLE") == 0)
    {
        k = OK_BINFLOAT;
        r = "double precision";
    }
    else if (strcmp(dt, "VARCHAR2") == 0 || strcmp(dt, "NVARCHAR2") == 0 ||
             strcmp(dt, "VARCHAR") == 0)
    {
        int n = char_length > 0 ? char_length : data_length;

        k = OK_TEXT;
        r = n > 0 ? psprintf("varchar(%d)", n) : "text";
    }
    else if (strcmp(dt, "CHAR") == 0 || strcmp(dt, "NCHAR") == 0)
    {
        int n = char_length > 0 ? char_length : data_length;

        k = OK_TEXT;
        r = n > 0 ? psprintf("char(%d)", n) : "char(1)";
    }
    else if (strcmp(dt, "CLOB") == 0 || strcmp(dt, "NCLOB") == 0)
    {
        k = OK_CLOB;
        r = "text";
    }
    else if (strcmp(dt, "DATE") == 0)
    {
        k = OK_DATE;
        r = "timestamp(0)";
    }
    else if (strncmp(dt, "TIMESTAMP", 9) == 0)
    {
        /* Oracle allows 0..9 fractional digits, PostgreSQL 0..6. */
        int p = (scale >= 0) ? Min(scale, 6) : 6;

        if (strstr(dt, "WITH LOCAL TIME ZONE"))
        {
            k = OK_TSLTZ;
            r = psprintf("timestamptz(%d)", p);
        }
        else if (strstr(dt, "WITH TIME ZONE"))
        {
            k = OK_TSTZ;
            r = psprintf("timestamptz(%d)", p);
        }
        else
        {
            k = OK_TS;
            r = psprintf("timestamp(%d)", p);
        }
    }
    else if (strcmp(dt, "RAW") == 0)
    {
        k = OK_RAW;
        r = "bytea";
    }
    else if (strcmp(dt, "ROWID") == 0)
    {
        k = OK_ROWID;
        r = "varchar(18)";
    }
    /* Everything else (BLOB, BFILE, LONG, LONG RAW, XMLTYPE, INTERVAL *,
     * UROWID, object/collection types) is not supported yet. */

    if (kind)
        *kind = k;
    return r;
}

/*
 * SQL-callable wrapper so the mapping is testable without an Oracle client.
 * For TIMESTAMP types pass the fractional-seconds precision as data_scale
 * (that is where ALL_TAB_COLUMNS reports it).
 */
Datum
pgclone_oracle_type_to_pg(PG_FUNCTION_ARGS)
{
    char       *dt;
    const char *r;
    char       *p;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();
    dt = text_to_cstring(PG_GETARG_TEXT_PP(0));
    for (p = dt; *p; p++)
        *p = pg_toupper((unsigned char) *p);

    r = map_oracle_type(dt,
                        PG_ARGISNULL(1) ? -1 : PG_GETARG_INT32(1),
                        PG_ARGISNULL(2) ? -1 : PG_GETARG_INT32(2),
                        PG_ARGISNULL(3) ? -1 : PG_GETARG_INT32(3),
                        PG_ARGISNULL(4) ? -1 : PG_GETARG_INT32(4),
                        NULL);
    if (r == NULL)
        PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(r));
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */
typedef struct OraColumn
{
    char       *ora_name;
    char       *ora_type;
    char       *pg_name;
    const char *pg_type;
    OraKind     kind;
    bool        notnull;
    bool        skip;        /* unsupported type */
    char       *select_expr;
    int         fetch_kind;  /* ORA_COL_TEXT / ORA_COL_BINARY */
    unsigned    bufsz;
} OraColumn;

/* 'text' -> 'te''xt' (Oracle string literal) */
static char *
ora_literal(const char *s)
{
    StringInfoData b;

    initStringInfo(&b);
    appendStringInfoChar(&b, '\'');
    for (; *s; s++)
    {
        if (*s == '\'')
            appendStringInfoChar(&b, '\'');
        appendStringInfoChar(&b, *s);
    }
    appendStringInfoChar(&b, '\'');
    return b.data;
}

/* name -> "NAME" (Oracle quoted identifier) */
static char *
ora_ident(const char *s)
{
    StringInfoData b;

    initStringInfo(&b);
    appendStringInfoChar(&b, '"');
    for (; *s; s++)
    {
        if (*s == '"')
            appendStringInfoChar(&b, '"');
        appendStringInfoChar(&b, *s);
    }
    appendStringInfoChar(&b, '"');
    return b.data;
}

static char *
lower_ascii(const char *s)
{
    char *r = pstrdup(s);
    char *p;

    for (p = r; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
    return r;
}

/* Parse "key=value key='quoted value' ..." (libpq style). */
typedef struct OraConnSpec
{
    char *host, *port, *service, *user, *password, *dsn;
} OraConnSpec;

static void
parse_oracle_conninfo(const char *ci, OraConnSpec *o)
{
    const char *p = ci;

    memset(o, 0, sizeof(*o));
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
                     errmsg("pgclone: invalid Oracle conninfo near \"%s\"", key.data),
                     errhint("Use: host=H port=1521 service=SVC user=U password=P  (or dsn=TNS_ALIAS user=U password=P)")));
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

        if (strcmp(key.data, "host") == 0)                o->host = val.data;
        else if (strcmp(key.data, "port") == 0)           o->port = val.data;
        else if (strcmp(key.data, "service") == 0 ||
                 strcmp(key.data, "service_name") == 0)   o->service = val.data;
        else if (strcmp(key.data, "user") == 0)           o->user = val.data;
        else if (strcmp(key.data, "password") == 0)       o->password = val.data;
        else if (strcmp(key.data, "dsn") == 0)            o->dsn = val.data;
        else
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("pgclone: unknown Oracle conninfo key \"%s\"", key.data)));
    }

    if (o->password == NULL)
    {
        /* Keep secrets out of SQL text / logs when possible. */
        const char *env = getenv("PGCLONE_ORACLE_PASSWORD");

        if (env)
            o->password = pstrdup(env);
    }
    if (!o->user || !o->password)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone: Oracle conninfo needs user and password"),
                 errhint("password may instead come from the PGCLONE_ORACLE_PASSWORD environment variable of the server process.")));
    if (!o->dsn && !(o->host && o->service))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone: Oracle conninfo needs host and service (or dsn)")));
}

static char *
oracle_connect_string(const OraConnSpec *o)
{
    if (o->dsn)
        return pstrdup(o->dsn);
    return psprintf("//%s:%s/%s", o->host, o->port ? o->port : "1521", o->service);
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

static void
copy_hex(StringInfo b, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    appendStringInfoString(b, "\\\\x");     /* \\x after COPY unescaping = \x */
    for (i = 0; i < len; i++)
    {
        appendStringInfoChar(b, hex[((unsigned char) s[i]) >> 4]);
        appendStringInfoChar(b, hex[((unsigned char) s[i]) & 15]);
    }
}

#define ORA_FAIL(buf) \
    ereport(ERROR, \
            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), \
             errmsg("pgclone: Oracle: %s", (buf))))

/* ------------------------------------------------------------------ */
/* Metadata                                                           */
/* ------------------------------------------------------------------ */
static int
fetch_columns(OraConn *conn, const char *schema, const char *table,
              OraColumn **out)
{
    static const int      kinds[7] = {0, 0, 0, 0, 0, 0, 0};
    static const unsigned bufsz[7] = {600, 130, 16, 16, 16, 4, 16};
    char        err[ORA_ERRLEN];
    StringInfoData q;
    OraCursor  *cur;
    OraColumn  *cols = NULL;
    int         n = 0, cap = 0, rc;

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT column_name, data_type, TO_CHAR(data_length), "
        "TO_CHAR(data_precision), TO_CHAR(data_scale), nullable, "
        "TO_CHAR(char_length) "
        "FROM all_tab_columns WHERE owner = %s AND table_name = %s "
        "ORDER BY column_id",   /* ALL_TAB_COLUMNS already omits hidden columns */
        ora_literal(schema), ora_literal(table));

    cur = ora_open(conn, q.data, 7, kinds, bufsz, err, sizeof(err));
    if (!cur)
        ORA_FAIL(err);

    while ((rc = ora_fetch(cur, err, sizeof(err))) == 1)
    {
        OraColumn *c;
        const char *v[7];
        int         i;
        size_t      l;

        if (n == cap)
        {
            cap = cap ? cap * 2 : 32;
            cols = cols ? repalloc(cols, cap * sizeof(OraColumn))
                        : palloc(cap * sizeof(OraColumn));
        }
        for (i = 0; i < 7; i++)
        {
            const char *raw = ora_value(cur, i, &l);

            v[i] = raw ? pnstrdup(raw, l) : NULL;
        }
        c = &cols[n++];
        memset(c, 0, sizeof(*c));
        c->ora_name = (char *) v[0];
        c->ora_type = (char *) v[1];
        c->notnull = (v[5] && v[5][0] == 'N');
        c->pg_type = map_oracle_type(c->ora_type,
                                     v[2] ? atoi(v[2]) : -1,
                                     v[3] ? atoi(v[3]) : -1,
                                     v[4] ? atoi(v[4]) : -1,
                                     v[6] ? atoi(v[6]) : -1,
                                     &c->kind);
        c->skip = (c->pg_type == NULL);
        c->pg_name = lower_ascii(c->ora_name);

        if (!c->skip)
        {
            char *id = ora_ident(c->ora_name);
            int   dlen = v[2] ? atoi(v[2]) : 0;
            int   clen = v[6] ? atoi(v[6]) : 0;

            c->fetch_kind = ORA_COL_TEXT;
            switch (c->kind)
            {
                case OK_TEXT:
                    c->select_expr = id;
                    /* UTF-8 can need up to 4 bytes per character. */
                    c->bufsz = Max(dlen, clen * 4) + 4;
                    break;
                case OK_CLOB:
                    c->select_expr = id;
                    c->bufsz = ORA_LOB_MAX_BYTES;
                    break;
                case OK_NUMBER:
                    c->select_expr = psprintf("TO_CHAR(%s, 'TM9')", id);
                    c->bufsz = 160;
                    break;
                case OK_BINFLOAT:
                    c->select_expr = psprintf("TO_CHAR(%s)", id);
                    c->bufsz = 80;
                    break;
                case OK_DATE:
                    c->select_expr = psprintf("TO_CHAR(%s, 'YYYY-MM-DD HH24:MI:SS')", id);
                    c->bufsz = 40;
                    break;
                case OK_TS:
                    c->select_expr = psprintf("TO_CHAR(%s, 'YYYY-MM-DD HH24:MI:SS.FF6')", id);
                    c->bufsz = 48;
                    break;
                case OK_TSTZ:
                    c->select_expr = psprintf("TO_CHAR(%s, 'YYYY-MM-DD HH24:MI:SS.FF6 TZH:TZM')", id);
                    c->bufsz = 64;
                    break;
                case OK_TSLTZ:
                    /* session time zone is forced to UTC by the caller */
                    c->select_expr = psprintf("TO_CHAR(CAST(%s AS TIMESTAMP WITH TIME ZONE), 'YYYY-MM-DD HH24:MI:SS.FF6 TZH:TZM')", id);
                    c->bufsz = 64;
                    break;
                case OK_RAW:
                    c->select_expr = id;
                    c->fetch_kind = ORA_COL_BINARY;
                    c->bufsz = Max(dlen, 1) + 4;
                    break;
                case OK_ROWID:
                    c->select_expr = psprintf("ROWIDTOCHAR(%s)", id);
                    c->bufsz = 64;
                    break;
                default:
                    c->skip = true;
            }
        }
    }
    if (rc < 0)
    {
        ora_close(cur);
        ORA_FAIL(err);
    }
    ora_close(cur);
    *out = cols;
    return n;
}

/*
 * Build ", PRIMARY KEY (...), UNIQUE (...)" clauses. Constraints touching a
 * skipped (unsupported) column are dropped with a NOTICE.
 */
static void
append_constraints(OraConn *conn, const char *schema, const char *table,
                   OraColumn *cols, int ncols, StringInfo ddl)
{
    static const int      kinds[3] = {0, 0, 0};
    static const unsigned bufsz[3] = {4, 600, 600};
    char        err[ORA_ERRLEN];
    StringInfoData q, cur_cols;
    OraCursor  *cur;
    char       *cur_name = NULL;
    char        cur_type = 0;
    bool        cur_ok = true;
    int         rc;

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT c.constraint_type, c.constraint_name, cc.column_name "
        "FROM all_constraints c JOIN all_cons_columns cc "
        "ON cc.owner = c.owner AND cc.constraint_name = c.constraint_name "
        "WHERE c.owner = %s AND c.table_name = %s "
        "AND c.constraint_type IN ('P','U') AND c.status = 'ENABLED' "
        "ORDER BY DECODE(c.constraint_type,'P',0,1), c.constraint_name, cc.position",
        ora_literal(schema), ora_literal(table));

    cur = ora_open(conn, q.data, 3, kinds, bufsz, err, sizeof(err));
    if (!cur)
        ORA_FAIL(err);

    initStringInfo(&cur_cols);

#define FLUSH_CONSTRAINT() \
    do { \
        if (cur_name) \
        { \
            if (cur_ok) \
                appendStringInfo(ddl, ", %s (%s)", \
                                 cur_type == 'P' ? "PRIMARY KEY" : "UNIQUE", \
                                 cur_cols.data); \
            else \
                ereport(NOTICE, (errmsg("pgclone: skipping Oracle constraint %s " \
                                        "(involves an unsupported column)", cur_name))); \
        } \
    } while (0)

    while ((rc = ora_fetch(cur, err, sizeof(err))) == 1)
    {
        size_t      l;
        const char *t = ora_value(cur, 0, &l);
        const char *nm = pnstrdup(ora_value(cur, 1, &l), l);
        const char *cn = pnstrdup(ora_value(cur, 2, &l), l);
        int         i;
        OraColumn  *found = NULL;

        if (cur_name == NULL || strcmp(cur_name, nm) != 0)
        {
            FLUSH_CONSTRAINT();
            cur_name = (char *) nm;
            cur_type = t[0];
            cur_ok = true;
            resetStringInfo(&cur_cols);
        }
        for (i = 0; i < ncols; i++)
            if (strcmp(cols[i].ora_name, cn) == 0)
                found = &cols[i];
        if (!found || found->skip)
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
        ora_close(cur);
        ORA_FAIL(err);
    }
    FLUSH_CONSTRAINT();
    ora_close(cur);
#undef FLUSH_CONSTRAINT
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                   */
/* ------------------------------------------------------------------ */
Datum
pgclone_table_from_oracle(PG_FUNCTION_ARGS)
{
    char       *conninfo, *ora_schema, *ora_table;
    bool        include_data, dry_run;
    char       *tgt_schema, *tgt_table;
    OraConnSpec spec;
    char        err[ORA_ERRLEN];
    OraConn *volatile oconn = NULL;
    OraCursor *volatile cursor = NULL;
    PGconn   *volatile lconn = NULL;
    volatile bool in_txn = false;
    OraColumn  *cols = NULL;
    int         ncols, i, nsel = 0;
    StringInfoData ddl, sel, cmd;
    int64       rows = 0;
    char       *result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("pgclone: oracle_conninfo, oracle_schema and oracle_table are required")));

    conninfo     = text_to_cstring(PG_GETARG_TEXT_PP(0));
    ora_schema   = text_to_cstring(PG_GETARG_TEXT_PP(1));
    ora_table    = text_to_cstring(PG_GETARG_TEXT_PP(2));
    include_data = PG_ARGISNULL(3) ? true : PG_GETARG_BOOL(3);
    tgt_schema   = PG_ARGISNULL(4) ? "public" : text_to_cstring(PG_GETARG_TEXT_PP(4));
    tgt_table    = PG_ARGISNULL(5) ? lower_ascii(ora_table)
                                   : text_to_cstring(PG_GETARG_TEXT_PP(5));
    dry_run      = PG_ARGISNULL(6) ? false : PG_GETARG_BOOL(6);

    if (!ora_is_available())
    {
        ora_connect("", "", "", err, sizeof(err));   /* fills the stub message */
        ORA_FAIL(err);
    }

    parse_oracle_conninfo(conninfo, &spec);

    PG_TRY();
    {
        oconn = ora_connect(oracle_connect_string(&spec), spec.user,
                            spec.password, err, sizeof(err));
        if (!oconn)
            ORA_FAIL(err);

        /* Deterministic text formats + one consistent snapshot. */
        if (ora_exec(oconn, "ALTER SESSION SET NLS_NUMERIC_CHARACTERS = '.,'", err, sizeof(err)) < 0 ||
            ora_exec(oconn, "ALTER SESSION SET TIME_ZONE = '+00:00'", err, sizeof(err)) < 0 ||
            ora_exec(oconn, "SET TRANSACTION READ ONLY", err, sizeof(err)) < 0)
            ORA_FAIL(err);

        ncols = fetch_columns(oconn, ora_schema, ora_table, &cols);
        if (ncols == 0)
        {
            /* Unquoted Oracle identifiers are stored upper-case. */
            ora_schema = pstrdup(ora_schema);
            ora_table = pstrdup(ora_table);
            for (char *p = ora_schema; *p; p++) *p = pg_toupper((unsigned char) *p);
            for (char *p = ora_table; *p; p++) *p = pg_toupper((unsigned char) *p);
            ncols = fetch_columns(oconn, ora_schema, ora_table, &cols);
        }
        if (ncols == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg("pgclone: Oracle table %s.%s not found or not visible to this user",
                            ora_schema, ora_table)));

        /* Duplicate PG names (e.g. "Id" and "ID" in Oracle) would collide. */
        for (i = 0; i < ncols; i++)
        {
            int j;

            for (j = 0; j < i; j++)
                if (!cols[i].skip && !cols[j].skip &&
                    strcmp(cols[i].pg_name, cols[j].pg_name) == 0)
                    ereport(ERROR,
                            (errcode(ERRCODE_DUPLICATE_COLUMN),
                             errmsg("pgclone: Oracle columns \"%s\" and \"%s\" both map to PostgreSQL column \"%s\"",
                                    cols[j].ora_name, cols[i].ora_name, cols[i].pg_name)));
            if (cols[i].skip)
                ereport(WARNING,
                        (errmsg("pgclone: skipping column %s: Oracle type %s is not supported",
                                cols[i].ora_name, cols[i].ora_type)));
            else
                nsel++;
        }
        if (nsel == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("pgclone: no column of %s.%s has a supported type", ora_schema, ora_table)));

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
        append_constraints(oconn, ora_schema, ora_table, cols, ncols, &ddl);
        appendStringInfoChar(&ddl, ')');

        if (dry_run)
        {
            result = psprintf("-- dry run, nothing executed\n%s;", ddl.data);
        }
        else
        {
            PGresult *res;
            int       cret;
            int       *kinds = palloc(nsel * sizeof(int));
            unsigned  *sizes = palloc(nsel * sizeof(unsigned));
            int        k = 0;

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
                    kinds[k] = cols[i].fetch_kind;
                    sizes[k] = cols[i].bufsz;
                    k++;
                }
                appendStringInfo(&sel, " FROM %s.%s", ora_ident(ora_schema), ora_ident(ora_table));
                appendStringInfoString(&cmd, ") FROM STDIN WITH (FORMAT text)");

                cursor = ora_open(oconn, sel.data, nsel, kinds, sizes, err, sizeof(err));
                if (!cursor)
                    ORA_FAIL(err);

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
                    while ((fr = ora_fetch(cursor, err, sizeof(err))) == 1)
                    {
                        for (i = 0, k = 0; i < ncols; i++)
                        {
                            size_t      vl = 0;
                            const char *v;

                            if (cols[i].skip)
                                continue;
                            if (k > 0)
                                appendStringInfoChar(&row, '\t');
                            v = ora_value(cursor, k, &vl);
                            if (v == NULL)
                                appendStringInfoString(&row, "\\N");
                            else if (cols[i].fetch_kind == ORA_COL_BINARY)
                                copy_hex(&row, v, vl);
                            else
                                copy_escape(&row, v, vl);
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
                        PQputCopyEnd(lconn, "oracle fetch error");
                        ORA_FAIL(err);
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

            result = psprintf("Cloned Oracle %s.%s -> %s.%s (%d columns, %lld rows)",
                              ora_schema, ora_table, tgt_schema, tgt_table,
                              nsel, (long long) rows);
        }
    }
    PG_CATCH();
    {
        if (cursor)
            ora_close(cursor);
        if (oconn)
            ora_disconnect(oconn);
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
        ora_close(cursor);
    if (oconn)
        ora_disconnect(oconn);
    if (lconn)
        PQfinish(lconn);

    PG_RETURN_TEXT_P(cstring_to_text(result));
}
