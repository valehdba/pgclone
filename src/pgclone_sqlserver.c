/*
 * pgclone_sqlserver.c - clone a table from Microsoft SQL Server into PostgreSQL.
 *
 *   pgclone.table_from_sqlserver(sqlserver_conninfo, sqlserver_schema,
 *                                sqlserver_table, include_data,
 *                                target_schema, target_table, dry_run)
 *   pgclone.sqlserver_type_to_pg(data_type, max_length, "precision", scale)
 *
 * Flow: read column + PK/UNIQUE metadata from sys.columns / sys.indexes, map
 * SQL Server types to PostgreSQL types, then (on a loopback connection,
 * inside one transaction) CREATE TABLE and stream the rows with
 * COPY ... FROM STDIN. SQL Server access goes through pgclone_tds.c (FreeTDS
 * DB-Library), which is only functional when built with WITH_SQLSERVER=1.
 *
 * Every column is converted to text on the SQL Server side (CONVERT styles
 * with a fixed, locale-independent format), so the loader only deals with
 * character data.
 */
#include "postgres.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "libpq-fe.h"

#include "pgclone_tds.h"

extern PGconn *pgclone_connect_local(void);

#define SS_ERRLEN         1024
#define COPY_FLUSH_BYTES  (64 * 1024)

PG_FUNCTION_INFO_V1(pgclone_table_from_sqlserver);
PG_FUNCTION_INFO_V1(pgclone_sqlserver_type_to_pg);

/* ------------------------------------------------------------------ */
/* Type mapping                                                       */
/* ------------------------------------------------------------------ */
typedef enum SsKind
{
    SK_UNSUPPORTED = 0,
    SK_TEXT,        /* char / varchar / nchar / nvarchar / text / ntext / xml */
    SK_INT,         /* tinyint / smallint / int / bigint / bit / decimal / numeric */
    SK_MONEY,       /* money / smallmoney                               */
    SK_FLOAT,       /* real / float                                     */
    SK_DATE,        /* date                                             */
    SK_TIME,        /* time(n)                                          */
    SK_DATETIME,    /* datetime / smalldatetime / datetime2(n)          */
    SK_DTOFFSET,    /* datetimeoffset(n)                                */
    SK_UUID,        /* uniqueidentifier                                 */
    SK_BINARY       /* binary / varbinary / image / rowversion          */
} SsKind;

/*
 * max_length is in BYTES, as in sys.columns (-1 = MAX); prec / scale are
 * sys.columns.precision / scale. -1 means NULL.
 */
static const char *
map_sqlserver_type(const char *dt, int max_length, int prec, int scale,
                   SsKind *kind)
{
    SsKind      k = SK_UNSUPPORTED;
    const char *r = NULL;

    if (strcmp(dt, "bit") == 0)
    {
        k = SK_INT;
        r = "boolean";
    }
    else if (strcmp(dt, "tinyint") == 0 || strcmp(dt, "smallint") == 0)
    {
        k = SK_INT;
        r = "smallint";
    }
    else if (strcmp(dt, "int") == 0)
    {
        k = SK_INT;
        r = "integer";
    }
    else if (strcmp(dt, "bigint") == 0)
    {
        k = SK_INT;
        r = "bigint";
    }
    else if (strcmp(dt, "decimal") == 0 || strcmp(dt, "numeric") == 0)
    {
        k = SK_INT;
        if (prec < 0)
            r = "numeric";
        else if (scale > 0)
            r = psprintf("numeric(%d,%d)", Max(prec, scale), scale);
        else
            r = psprintf("numeric(%d)", prec);
    }
    else if (strcmp(dt, "money") == 0)
    {
        k = SK_MONEY;
        r = "numeric(19,4)";
    }
    else if (strcmp(dt, "smallmoney") == 0)
    {
        k = SK_MONEY;
        r = "numeric(10,4)";
    }
    else if (strcmp(dt, "real") == 0)
    {
        k = SK_FLOAT;
        r = "real";
    }
    else if (strcmp(dt, "float") == 0)
    {
        k = SK_FLOAT;
        r = (prec >= 0 && prec <= 24) ? "real" : "double precision";
    }
    else if (strcmp(dt, "char") == 0)
    {
        k = SK_TEXT;
        r = max_length > 0 ? psprintf("char(%d)", max_length) : "char(1)";
    }
    else if (strcmp(dt, "nchar") == 0)
    {
        k = SK_TEXT;
        r = max_length > 0 ? psprintf("char(%d)", max_length / 2) : "char(1)";
    }
    else if (strcmp(dt, "varchar") == 0)
    {
        k = SK_TEXT;
        r = max_length > 0 ? psprintf("varchar(%d)", max_length) : "text";
    }
    else if (strcmp(dt, "nvarchar") == 0)
    {
        k = SK_TEXT;
        r = max_length > 0 ? psprintf("varchar(%d)", max_length / 2) : "text";
    }
    else if (strcmp(dt, "text") == 0 || strcmp(dt, "ntext") == 0 ||
             strcmp(dt, "xml") == 0)
    {
        k = SK_TEXT;
        r = "text";
    }
    else if (strcmp(dt, "date") == 0)
    {
        k = SK_DATE;
        r = "date";
    }
    else if (strcmp(dt, "time") == 0)
    {
        /* SQL Server allows 0..7 fractional digits, PostgreSQL 0..6. */
        k = SK_TIME;
        r = psprintf("time(%d)", scale >= 0 ? Min(scale, 6) : 6);
    }
    else if (strcmp(dt, "datetime") == 0)
    {
        k = SK_DATETIME;
        r = "timestamp(3)";
    }
    else if (strcmp(dt, "smalldatetime") == 0)
    {
        k = SK_DATETIME;
        r = "timestamp(0)";
    }
    else if (strcmp(dt, "datetime2") == 0)
    {
        k = SK_DATETIME;
        r = psprintf("timestamp(%d)", scale >= 0 ? Min(scale, 6) : 6);
    }
    else if (strcmp(dt, "datetimeoffset") == 0)
    {
        k = SK_DTOFFSET;
        r = psprintf("timestamptz(%d)", scale >= 0 ? Min(scale, 6) : 6);
    }
    else if (strcmp(dt, "uniqueidentifier") == 0)
    {
        k = SK_UUID;
        r = "uuid";
    }
    else if (strcmp(dt, "binary") == 0 || strcmp(dt, "varbinary") == 0 ||
             strcmp(dt, "image") == 0 || strcmp(dt, "timestamp") == 0 ||
             strcmp(dt, "rowversion") == 0)
    {
        k = SK_BINARY;
        r = "bytea";
    }
    /* Everything else (sql_variant, hierarchyid, geography, geometry and
     * user-defined CLR types) is not supported yet. */

    if (kind)
        *kind = k;
    return r;
}

/*
 * SQL-callable wrapper so the mapping is testable without a SQL Server
 * client. max_length is in bytes (sys.columns.max_length); for time,
 * datetime2 and datetimeoffset pass the fractional-seconds precision as
 * "scale" (that is where sys.columns reports it).
 */
Datum
pgclone_sqlserver_type_to_pg(PG_FUNCTION_ARGS)
{
    char       *dt;
    const char *r;
    char       *p;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();
    dt = text_to_cstring(PG_GETARG_TEXT_PP(0));
    for (p = dt; *p; p++)
        *p = pg_tolower((unsigned char) *p);

    r = map_sqlserver_type(dt,
                           PG_ARGISNULL(1) ? -1 : PG_GETARG_INT32(1),
                           PG_ARGISNULL(2) ? -1 : PG_GETARG_INT32(2),
                           PG_ARGISNULL(3) ? -1 : PG_GETARG_INT32(3),
                           NULL);
    if (r == NULL)
        PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(r));
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */
typedef struct SsColumn
{
    char       *ss_name;
    char       *ss_type;
    char       *pg_name;
    const char *pg_type;
    SsKind      kind;
    bool        notnull;
    bool        skip;        /* unsupported type */
    char       *select_expr;
} SsColumn;

/* text -> N'te''xt' (T-SQL Unicode string literal) */
static char *
ss_literal(const char *s)
{
    StringInfoData b;

    initStringInfo(&b);
    appendStringInfoString(&b, "N'");
    for (; *s; s++)
    {
        if (*s == '\'')
            appendStringInfoChar(&b, '\'');
        appendStringInfoChar(&b, *s);
    }
    appendStringInfoChar(&b, '\'');
    return b.data;
}

/* name -> [name] (T-SQL bracketed identifier) */
static char *
ss_ident(const char *s)
{
    StringInfoData b;

    initStringInfo(&b);
    appendStringInfoChar(&b, '[');
    for (; *s; s++)
    {
        if (*s == ']')
            appendStringInfoChar(&b, ']');
        appendStringInfoChar(&b, *s);
    }
    appendStringInfoChar(&b, ']');
    return b.data;
}

static char *
ss_lower_ascii(const char *s)
{
    char *r = pstrdup(s);
    char *p;

    for (p = r; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
    return r;
}

/* Parse "key=value key='quoted value' ..." (libpq style). */
typedef struct SsConnSpec
{
    char *host, *port, *database, *user, *password;
    int   encrypt;           /* -1 = driver default */
} SsConnSpec;

static void
parse_sqlserver_conninfo(const char *ci, SsConnSpec *o)
{
    const char *p = ci;

    memset(o, 0, sizeof(*o));
    o->encrypt = -1;
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
                     errmsg("pgclone: invalid SQL Server conninfo near \"%s\"", key.data),
                     errhint("Use: host=H port=1433 database=DB user=U password=P")));
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

        if (strcmp(key.data, "host") == 0 || strcmp(key.data, "server") == 0)
            o->host = val.data;
        else if (strcmp(key.data, "port") == 0)
            o->port = val.data;
        else if (strcmp(key.data, "database") == 0 || strcmp(key.data, "dbname") == 0)
            o->database = val.data;
        else if (strcmp(key.data, "user") == 0)
            o->user = val.data;
        else if (strcmp(key.data, "password") == 0)
            o->password = val.data;
        else if (strcmp(key.data, "encrypt") == 0)
        {
            if (strcmp(val.data, "yes") == 0 || strcmp(val.data, "true") == 0 ||
                strcmp(val.data, "on") == 0 || strcmp(val.data, "require") == 0)
                o->encrypt = 1;
            else if (strcmp(val.data, "no") == 0 || strcmp(val.data, "false") == 0 ||
                     strcmp(val.data, "off") == 0)
                o->encrypt = 0;
            else
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("pgclone: invalid SQL Server conninfo value for encrypt: \"%s\"",
                                val.data),
                         errhint("Use encrypt=yes or encrypt=no.")));
        }
        else
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("pgclone: unknown SQL Server conninfo key \"%s\"", key.data)));
    }

    if (o->password == NULL)
    {
        /* Keep secrets out of SQL text / logs when possible. */
        const char *env = getenv("PGCLONE_SQLSERVER_PASSWORD");

        if (env)
            o->password = pstrdup(env);
    }
    if (!o->user || !o->password)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone: SQL Server conninfo needs user and password"),
                 errhint("password may instead come from the PGCLONE_SQLSERVER_PASSWORD environment variable of the server process.")));
    if (!o->host)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone: SQL Server conninfo needs host")));
    if (!o->port)
        o->port = "1433";
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

#define SS_FAIL(buf) \
    ereport(ERROR, \
            (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), \
             errmsg("pgclone: SQL Server: %s", (buf))))

/* ------------------------------------------------------------------ */
/* Metadata                                                           */
/* ------------------------------------------------------------------ */
static int
fetch_columns(TdsConn *conn, const char *schema, const char *table,
              SsColumn **out)
{
    char        err[SS_ERRLEN];
    StringInfoData q;
    TdsCursor  *cur;
    SsColumn   *cols = NULL;
    int         n = 0, cap = 0, rc;

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT CAST(c.name AS NVARCHAR(256)), "
        "CAST(CASE WHEN t.is_assembly_type = 1 THEN t.name "
        "ELSE TYPE_NAME(c.system_type_id) END AS NVARCHAR(128)), "
        "CAST(c.max_length AS VARCHAR(12)), CAST(c.precision AS VARCHAR(12)), "
        "CAST(c.scale AS VARCHAR(12)), CAST(c.is_nullable AS VARCHAR(2)) "
        "FROM sys.columns c "
        "JOIN sys.objects o ON o.object_id = c.object_id "
        "JOIN sys.schemas s ON s.schema_id = o.schema_id "
        "JOIN sys.types t ON t.user_type_id = c.user_type_id "
        "WHERE s.name = %s AND o.name = %s AND o.type IN ('U', 'V') "
        "ORDER BY c.column_id",
        ss_literal(schema), ss_literal(table));

    cur = tds_open(conn, q.data, 6, err, sizeof(err));
    if (!cur)
        SS_FAIL(err);

    while ((rc = tds_fetch(cur, err, sizeof(err))) == 1)
    {
        SsColumn   *c;
        const char *v[6];
        int         i;
        size_t      l;

        if (n == cap)
        {
            cap = cap ? cap * 2 : 32;
            cols = cols ? repalloc(cols, cap * sizeof(SsColumn))
                        : palloc(cap * sizeof(SsColumn));
        }
        for (i = 0; i < 6; i++)
        {
            const char *raw = tds_value(cur, i, &l);

            v[i] = raw ? pnstrdup(raw, l) : NULL;
        }
        c = &cols[n++];
        memset(c, 0, sizeof(*c));
        c->ss_name = (char *) v[0];
        c->ss_type = v[1] ? (char *) v[1] : "";
        c->notnull = (v[5] && v[5][0] == '0');
        c->pg_type = map_sqlserver_type(c->ss_type,
                                        v[2] ? atoi(v[2]) : -1,
                                        v[3] ? atoi(v[3]) : -1,
                                        v[4] ? atoi(v[4]) : -1,
                                        &c->kind);
        c->skip = (c->pg_type == NULL);
        c->pg_name = ss_lower_ascii(c->ss_name);

        if (!c->skip)
        {
            char *id = ss_ident(c->ss_name);

            switch (c->kind)
            {
                case SK_TEXT:
                    /* varchar goes through NVARCHAR so the server converts
                     * from its code page; FreeTDS then delivers UTF-8. */
                    c->select_expr = psprintf("CAST(%s AS NVARCHAR(MAX))", id);
                    break;
                case SK_INT:
                    c->select_expr = psprintf("CONVERT(VARCHAR(80), %s)", id);
                    break;
                case SK_MONEY:
                    c->select_expr = psprintf("CONVERT(VARCHAR(40), %s, 2)", id);
                    break;
                case SK_FLOAT:
                    /* style 3: round-trippable (9 / 17 significant digits) */
                    c->select_expr = psprintf("CONVERT(VARCHAR(40), %s, 3)", id);
                    break;
                case SK_DATE:
                    c->select_expr = psprintf("CONVERT(VARCHAR(10), %s, 23)", id);
                    break;
                case SK_TIME:
                    c->select_expr = psprintf("CONVERT(VARCHAR(20), %s, 121)", id);
                    break;
                case SK_DATETIME:
                    c->select_expr = psprintf("CONVERT(VARCHAR(30), %s, 121)", id);
                    break;
                case SK_DTOFFSET:
                    c->select_expr = psprintf("CONVERT(VARCHAR(40), %s, 127)", id);
                    break;
                case SK_UUID:
                    c->select_expr = psprintf("CONVERT(VARCHAR(36), %s)", id);
                    break;
                case SK_BINARY:
                    /* hex digits without the 0x prefix */
                    c->select_expr = psprintf("CONVERT(VARCHAR(MAX), CAST(%s AS VARBINARY(MAX)), 2)", id);
                    break;
                default:
                    c->skip = true;
            }
        }
    }
    if (rc < 0)
    {
        tds_close(cur);
        SS_FAIL(err);
    }
    tds_close(cur);
    *out = cols;
    return n;
}

/*
 * Build ", PRIMARY KEY (...), UNIQUE (...)" clauses. Constraints touching a
 * skipped (unsupported) column are dropped with a NOTICE.
 */
static void
append_constraints(TdsConn *conn, const char *schema, const char *table,
                   SsColumn *cols, int ncols, StringInfo ddl)
{
    char        err[SS_ERRLEN];
    StringInfoData q, cur_cols;
    TdsCursor  *cur;
    char       *cur_name = NULL;
    char        cur_type = 0;
    bool        cur_ok = true;
    int         rc;

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT CASE WHEN i.is_primary_key = 1 THEN 'P' ELSE 'U' END, "
        "CAST(i.name AS NVARCHAR(256)), CAST(c.name AS NVARCHAR(256)) "
        "FROM sys.indexes i "
        "JOIN sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id "
        "JOIN sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id "
        "JOIN sys.objects o ON o.object_id = i.object_id "
        "JOIN sys.schemas s ON s.schema_id = o.schema_id "
        "WHERE s.name = %s AND o.name = %s "
        "AND (i.is_primary_key = 1 OR i.is_unique_constraint = 1) "
        "AND i.is_disabled = 0 AND ic.is_included_column = 0 "
        "ORDER BY i.is_primary_key DESC, i.name, ic.key_ordinal",
        ss_literal(schema), ss_literal(table));

    cur = tds_open(conn, q.data, 3, err, sizeof(err));
    if (!cur)
        SS_FAIL(err);

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
                ereport(NOTICE, (errmsg("pgclone: skipping SQL Server constraint %s " \
                                        "(involves an unsupported column)", cur_name))); \
        } \
    } while (0)

    while ((rc = tds_fetch(cur, err, sizeof(err))) == 1)
    {
        size_t      l;
        const char *t = tds_value(cur, 0, &l);
        const char *nm = pnstrdup(tds_value(cur, 1, &l), l);
        const char *cn = pnstrdup(tds_value(cur, 2, &l), l);
        int         i;
        SsColumn   *found = NULL;

        if (cur_name == NULL || strcmp(cur_name, nm) != 0)
        {
            FLUSH_CONSTRAINT();
            cur_name = (char *) nm;
            cur_type = t[0];
            cur_ok = true;
            resetStringInfo(&cur_cols);
        }
        for (i = 0; i < ncols; i++)
            if (strcmp(cols[i].ss_name, cn) == 0)
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
        tds_close(cur);
        SS_FAIL(err);
    }
    FLUSH_CONSTRAINT();
    tds_close(cur);
#undef FLUSH_CONSTRAINT
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                   */
/* ------------------------------------------------------------------ */
Datum
pgclone_table_from_sqlserver(PG_FUNCTION_ARGS)
{
    char       *conninfo, *ss_schema, *ss_table;
    bool        include_data, dry_run;
    char       *tgt_schema, *tgt_table;
    SsConnSpec  spec;
    char        err[SS_ERRLEN];
    TdsConn *volatile sconn = NULL;
    TdsCursor *volatile cursor = NULL;
    PGconn   *volatile lconn = NULL;
    volatile bool in_txn = false;
    SsColumn   *cols = NULL;
    int         ncols, i, nsel = 0;
    StringInfoData ddl, sel, cmd;
    int64       rows = 0;
    char       *result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("pgclone: sqlserver_conninfo, sqlserver_schema and sqlserver_table are required")));

    conninfo     = text_to_cstring(PG_GETARG_TEXT_PP(0));
    ss_schema    = text_to_cstring(PG_GETARG_TEXT_PP(1));
    ss_table     = text_to_cstring(PG_GETARG_TEXT_PP(2));
    include_data = PG_ARGISNULL(3) ? true : PG_GETARG_BOOL(3);
    tgt_schema   = PG_ARGISNULL(4) ? "public" : text_to_cstring(PG_GETARG_TEXT_PP(4));
    tgt_table    = PG_ARGISNULL(5) ? ss_lower_ascii(ss_table)
                                   : text_to_cstring(PG_GETARG_TEXT_PP(5));
    dry_run      = PG_ARGISNULL(6) ? false : PG_GETARG_BOOL(6);

    if (!tds_is_available())
    {
        tds_connect("", "", "", "", "", -1, err, sizeof(err));   /* fills the stub message */
        SS_FAIL(err);
    }

    parse_sqlserver_conninfo(conninfo, &spec);

    PG_TRY();
    {
        sconn = tds_connect(spec.host, spec.port, spec.database, spec.user,
                            spec.password, spec.encrypt, err, sizeof(err));
        if (!sconn)
            SS_FAIL(err);

        /* Deterministic text formats; no row-count messages; no TEXT truncation. */
        if (tds_exec(sconn, "SET NOCOUNT ON; SET TEXTSIZE 2147483647; SET DATEFORMAT ymd",
                     err, sizeof(err)) < 0)
            SS_FAIL(err);

        /*
         * One consistent snapshot, when the database allows it. Without
         * ALLOW_SNAPSHOT_ISOLATION the copy reads at READ COMMITTED and rows
         * modified during a long copy may be seen inconsistently.
         */
        {
            TdsCursor *sc = tds_open(sconn,
                "SELECT CAST(snapshot_isolation_state AS VARCHAR(2)) "
                "FROM sys.databases WHERE database_id = DB_ID()",
                1, err, sizeof(err));
            bool       snap = false;

            if (!sc)
                SS_FAIL(err);
            if (tds_fetch(sc, err, sizeof(err)) == 1)
            {
                size_t      l = 0;
                const char *v = tds_value(sc, 0, &l);

                snap = (v && l > 0 && v[0] == '1');
            }
            tds_close(sc);
            if (snap)
            {
                if (tds_exec(sconn, "SET TRANSACTION ISOLATION LEVEL SNAPSHOT; BEGIN TRANSACTION",
                             err, sizeof(err)) < 0)
                    SS_FAIL(err);
            }
            else
                ereport(NOTICE,
                        (errmsg("pgclone: SQL Server snapshot isolation is not enabled for this database; "
                                "reading at READ COMMITTED (not a single consistent snapshot)")));
        }

        ncols = fetch_columns(sconn, ss_schema, ss_table, &cols);
        if (ncols == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg("pgclone: SQL Server table %s.%s not found or not visible to this user",
                            ss_schema, ss_table)));

        /* Duplicate PG names (e.g. "Id" and "ID" in a case-sensitive database) would collide. */
        for (i = 0; i < ncols; i++)
        {
            int j;

            for (j = 0; j < i; j++)
                if (!cols[i].skip && !cols[j].skip &&
                    strcmp(cols[i].pg_name, cols[j].pg_name) == 0)
                    ereport(ERROR,
                            (errcode(ERRCODE_DUPLICATE_COLUMN),
                             errmsg("pgclone: SQL Server columns \"%s\" and \"%s\" both map to PostgreSQL column \"%s\"",
                                    cols[j].ss_name, cols[i].ss_name, cols[i].pg_name)));
            if (cols[i].skip)
                ereport(WARNING,
                        (errmsg("pgclone: skipping column %s: SQL Server type %s is not supported",
                                cols[i].ss_name, cols[i].ss_type)));
            else
                nsel++;
        }
        if (nsel == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("pgclone: no column of %s.%s has a supported type", ss_schema, ss_table)));

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
        append_constraints(sconn, ss_schema, ss_table, cols, ncols, &ddl);
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
                appendStringInfo(&sel, " FROM %s.%s", ss_ident(ss_schema), ss_ident(ss_table));
                appendStringInfoString(&cmd, ") FROM STDIN WITH (FORMAT text)");

                cursor = tds_open(sconn, sel.data, nsel, err, sizeof(err));
                if (!cursor)
                    SS_FAIL(err);

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
                    while ((fr = tds_fetch(cursor, err, sizeof(err))) == 1)
                    {
                        for (i = 0, k = 0; i < ncols; i++)
                        {
                            size_t      vl = 0;
                            const char *v;

                            if (cols[i].skip)
                                continue;
                            if (k > 0)
                                appendStringInfoChar(&row, '\t');
                            v = tds_value(cursor, k, &vl);
                            if (v == NULL)
                                appendStringInfoString(&row, "\\N");
                            else if (cols[i].kind == SK_BINARY)
                            {
                                /* already hex digits: \\x after COPY unescaping = \x */
                                appendStringInfoString(&row, "\\\\x");
                                appendBinaryStringInfo(&row, v, (int) vl);
                            }
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
                        PQputCopyEnd(lconn, "sql server fetch error");
                        SS_FAIL(err);
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

            result = psprintf("Cloned SQL Server %s.%s -> %s.%s (%d columns, %lld rows)",
                              ss_schema, ss_table, tgt_schema, tgt_table,
                              nsel, (long long) rows);
        }
    }
    PG_CATCH();
    {
        if (cursor)
            tds_close(cursor);
        if (sconn)
            tds_disconnect(sconn);
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
        tds_close(cursor);
    if (sconn)
        tds_disconnect(sconn);
    if (lconn)
        PQfinish(lconn);

    PG_RETURN_TEXT_P(cstring_to_text(result));
}
