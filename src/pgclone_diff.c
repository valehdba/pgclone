/*
 * pgclone_diff.c — Schema drift / DDL diff between source and local target.
 *
 * Implements pgclone.diff(source_conninfo, schema_name): a read-only
 * comparison of catalog metadata. Returns a JSON document describing
 * objects present in only one side and modified objects on both sides.
 *
 * Categories compared (per schema):
 *   - tables (and per-table column drift)
 *   - indexes (excluding those backing constraints)
 *   - constraints
 *   - triggers (user-defined only; AI/RI internal triggers excluded)
 *   - views and materialized views
 *   - sequences
 *
 * Both connections run in READ ONLY transactions; this function
 * never executes DDL or DML on either side.
 *
 * Copyright (c) 2026, Valeh Agayev pgclone contributors
 * Licensed under PostgreSQL License
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/jsonapi.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "commands/dbcommands.h"

/* ---------------------------------------------------------------
 * Forward decls of local helpers borrowed in spirit from pgclone.c.
 * We do NOT reuse symbols from pgclone.c — keeping this translation
 * unit fully self-contained makes the feature trivially auditable
 * and reversible.
 * --------------------------------------------------------------- */
static PGconn *pgclone_diff_connect_source(const char *conninfo);
static PGconn *pgclone_diff_connect_local(void);
static void    pgclone_diff_normalize_session(PGconn *conn);
static void    pgclone_diff_begin_readonly(PGconn *conn);
static void    pgclone_diff_rollback(PGconn *conn);
static PGresult *pgclone_diff_select(PGconn *conn, const char *query);
static void    pgclone_diff_validate_identifier(const char *ident, const char *what);
static void    pgclone_diff_append_local_host(StringInfo conninfo);

/* Per-category JSON emitters */
static void diff_tables_and_columns(PGconn *src, PGconn *tgt, const char *schema,
                                    StringInfo out, int *out_diff_count,
                                    int *only_src, int *only_tgt, int *modified);
static void diff_named_objects(PGconn *src, PGconn *tgt, const char *category_label,
                               const char *query, StringInfo out, int *out_diff_count,
                               int *only_src, int *only_tgt, int *modified);
static void diff_sequences(PGconn *src, PGconn *tgt, const char *schema,
                           StringInfo out, int *out_diff_count,
                           int *only_src, int *only_tgt);

/* ===============================================================
 * Connection helpers (self-contained copy of the same logic in
 * pgclone.c so this file can be compiled and reasoned about
 * independently). All errors raise via ereport(ERROR, ...).
 * =============================================================== */

static void
pgclone_diff_normalize_session(PGconn *conn)
{
    PGresult *res = PQexec(conn, "SET search_path = pg_catalog");
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
        ereport(WARNING,
                (errmsg("pgclone.diff: could not set search_path: %s",
                        PQerrorMessage(conn))));
    PQclear(res);
}

static void
pgclone_diff_begin_readonly(PGconn *conn)
{
    PGresult *res = PQexec(conn,
        "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        char *msg = pstrdup(PQerrorMessage(conn));
        PQclear(res);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone.diff: could not begin read-only transaction: %s", msg)));
    }
    PQclear(res);
}

static void
pgclone_diff_rollback(PGconn *conn)
{
    PGresult *res = PQexec(conn, "ROLLBACK");
    /* ROLLBACK shouldn't fail on a read-only tx; ignore status, just free. */
    if (res)
        PQclear(res);
}

static PGconn *
pgclone_diff_connect_source(const char *conninfo)
{
    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK)
    {
        char *msg = pstrdup(PQerrorMessage(conn));
        PQfinish(conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgclone.diff: could not connect to source: %s", msg)));
    }
    pgclone_diff_normalize_session(conn);
    pgclone_diff_begin_readonly(conn);
    return conn;
}

static void
pgclone_diff_append_local_host(StringInfo conninfo)
{
    const char *socket_dir = GetConfigOption("unix_socket_directories", false, false);

    if (socket_dir && socket_dir[0])
    {
        char *first_dir = pstrdup(socket_dir);
        char *comma = strchr(first_dir, ',');
        int   len;

        if (comma)
            *comma = '\0';
        len = (int) strlen(first_dir);
        while (len > 0 && first_dir[len - 1] == ' ')
            first_dir[--len] = '\0';
        appendStringInfo(conninfo, "host=%s", first_dir);
        pfree(first_dir);
    }
    else
    {
        appendStringInfoString(conninfo, "host=127.0.0.1");
    }
}

static PGconn *
pgclone_diff_connect_local(void)
{
    PGconn         *conn;
    StringInfoData  conninfo;
    const char     *dbname;
    const char     *port;
    const char     *username;

    dbname   = get_database_name(MyDatabaseId);
    port     = GetConfigOption("port", false, false);
    username = GetUserNameFromId(GetUserId(), false);

    initStringInfo(&conninfo);
    pgclone_diff_append_local_host(&conninfo);
    appendStringInfo(&conninfo, " dbname=%s port=%s user=%s",
                     quote_literal_cstr(dbname),
                     port ? port : "5432",
                     username);

    conn = PQconnectdb(conninfo.data);
    pfree(conninfo.data);

    if (PQstatus(conn) != CONNECTION_OK)
    {
        char *msg = pstrdup(PQerrorMessage(conn));
        PQfinish(conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgclone.diff: could not connect to local database: %s", msg)));
    }

    pgclone_diff_normalize_session(conn);
    pgclone_diff_begin_readonly(conn);
    return conn;
}

static PGresult *
pgclone_diff_select(PGconn *conn, const char *query)
{
    PGresult *res = PQexec(conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        char *msg = pstrdup(PQerrorMessage(conn));
        PQclear(res);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone.diff: catalog query failed: %s", msg)));
    }
    return res;
}

/* ---------------------------------------------------------------
 * Identifier validator. Defense-in-depth: even though we always
 * use quote_literal_cstr() to embed the schema name in catalog
 * filters, we still reject obviously hostile inputs (NULs, long
 * strings, non-printables) up front.
 * --------------------------------------------------------------- */
static void
pgclone_diff_validate_identifier(const char *ident, const char *what)
{
    size_t len;
    size_t i;

    if (ident == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone.diff: %s must not be NULL", what)));

    len = strlen(ident);
    if (len == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone.diff: %s must not be empty", what)));
    if (len >= NAMEDATALEN)
        ereport(ERROR,
                (errcode(ERRCODE_NAME_TOO_LONG),
                 errmsg("pgclone.diff: %s exceeds %d bytes",
                        what, NAMEDATALEN - 1)));

    for (i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char) ident[i];
        if (c < 0x20 || c == 0x7F)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("pgclone.diff: %s contains a control character", what)));
    }
}

/* ---------------------------------------------------------------
 * JSON helpers. We use PostgreSQL's escape_json() for strings.
 * For NULL-able values we emit the JSON literal `null` directly.
 * --------------------------------------------------------------- */
static void
emit_json_str(StringInfo out, const char *s)
{
    if (s == NULL)
        appendStringInfoString(out, "null");
    else
        escape_json(out, s);
}

static void
emit_json_pair_str(StringInfo out, const char *key, const char *val, bool first)
{
    if (!first)
        appendStringInfoChar(out, ',');
    appendStringInfoChar(out, '"');
    appendStringInfoString(out, key);
    appendStringInfoString(out, "\":");
    emit_json_str(out, val);
}

static void
emit_json_pair_bool(StringInfo out, const char *key, bool val, bool first)
{
    if (!first)
        appendStringInfoChar(out, ',');
    appendStringInfo(out, "\"%s\":%s", key, val ? "true" : "false");
}

static void
emit_json_pair_int(StringInfo out, const char *key, int val, bool first)
{
    if (!first)
        appendStringInfoChar(out, ',');
    appendStringInfo(out, "\"%s\":%d", key, val);
}

/* ---------------------------------------------------------------
 * Compare PG boolean text ('t'/'f') as bool.
 * --------------------------------------------------------------- */
static bool
pg_bool_value(const char *txt)
{
    return txt != NULL && txt[0] == 't';
}

/* ---------------------------------------------------------------
 * Find the contiguous range [start, end) of rows whose first column
 * equals the value at row `start`. Catalog queries ORDER BY that
 * column COLLATE "C", guaranteeing all matching rows are adjacent.
 * --------------------------------------------------------------- */
static int
range_end(PGresult *res, int start)
{
    int        n   = PQntuples(res);
    const char *k0 = PQgetvalue(res, start, 0);
    int        e   = start + 1;
    while (e < n && strcmp(PQgetvalue(res, e, 0), k0) == 0)
        e++;
    return e;
}

/* ===============================================================
 * Tables + per-table column drift.
 *
 * Catalog query (run identically against source and target):
 *   SELECT relname, attname, format_type(atttypid, atttypmod),
 *          attnotnull, pg_get_expr(adbin, adrelid)
 *   FROM   pg_class c
 *   JOIN   pg_namespace n ON n.oid = c.relnamespace
 *   JOIN   pg_attribute a ON a.attrelid = c.oid
 *   LEFT   JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum
 *   WHERE  n.nspname = $schema
 *     AND  c.relkind IN ('r','p')          -- regular + partitioned
 *     AND  a.attnum > 0 AND NOT a.attisdropped
 *   ORDER  BY relname COLLATE "C", attname COLLATE "C"
 *
 * Output JSON shape:
 *   "tables": {
 *     "only_in_source": ["t1", ...],
 *     "only_in_target": ["t2", ...],
 *     "modified": [
 *       { "name":"t3",
 *         "columns_only_in_source": [{"name":"c","type":"int","not_null":false,"default":null}],
 *         "columns_only_in_target": [...],
 *         "columns_drift":          [{"name":"c","source_type":"int","target_type":"bigint",
 *                                     "source_not_null":true,"target_not_null":true,
 *                                     "source_default":null,"target_default":"0"}]
 *       }
 *     ]
 *   }
 * =============================================================== */
static const char *
build_columns_query(StringInfo q, const char *schema)
{
    initStringInfo(q);
    appendStringInfo(q,
        "SELECT c.relname, a.attname, "
        "       format_type(a.atttypid, a.atttypmod), "
        "       a.attnotnull, "
        "       pg_get_expr(d.adbin, d.adrelid) "
        "FROM pg_catalog.pg_class c "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid "
        "LEFT JOIN pg_catalog.pg_attrdef d "
        "       ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
        "WHERE n.nspname = %s "
        "AND   c.relkind IN ('r','p') "
        "AND   a.attnum > 0 AND NOT a.attisdropped "
        "ORDER BY c.relname COLLATE \"C\", a.attname COLLATE \"C\"",
        quote_literal_cstr(schema));
    return q->data;
}

static void
emit_column_object(StringInfo out, PGresult *res, int row, bool first)
{
    if (!first)
        appendStringInfoChar(out, ',');
    appendStringInfoChar(out, '{');
    emit_json_pair_str (out, "name",     PQgetvalue(res, row, 1), true);
    emit_json_pair_str (out, "type",     PQgetvalue(res, row, 2), false);
    emit_json_pair_bool(out, "not_null", pg_bool_value(PQgetvalue(res, row, 3)), false);
    if (PQgetisnull(res, row, 4))
        appendStringInfoString(out, ",\"default\":null");
    else
        emit_json_pair_str(out, "default", PQgetvalue(res, row, 4), false);
    appendStringInfoChar(out, '}');
}

static void
emit_drift_object(StringInfo out, PGresult *src, int si, PGresult *tgt, int ti, bool first)
{
    if (!first)
        appendStringInfoChar(out, ',');
    appendStringInfoChar(out, '{');
    emit_json_pair_str (out, "name",            PQgetvalue(src, si, 1), true);
    emit_json_pair_str (out, "source_type",     PQgetvalue(src, si, 2), false);
    emit_json_pair_str (out, "target_type",     PQgetvalue(tgt, ti, 2), false);
    emit_json_pair_bool(out, "source_not_null", pg_bool_value(PQgetvalue(src, si, 3)), false);
    emit_json_pair_bool(out, "target_not_null", pg_bool_value(PQgetvalue(tgt, ti, 3)), false);
    appendStringInfoString(out, ",\"source_default\":");
    if (PQgetisnull(src, si, 4)) appendStringInfoString(out, "null");
    else emit_json_str(out, PQgetvalue(src, si, 4));
    appendStringInfoString(out, ",\"target_default\":");
    if (PQgetisnull(tgt, ti, 4)) appendStringInfoString(out, "null");
    else emit_json_str(out, PQgetvalue(tgt, ti, 4));
    appendStringInfoChar(out, '}');
}

static bool
column_attrs_equal(PGresult *a, int ai, PGresult *b, int bi)
{
    /* type, not_null, default */
    if (strcmp(PQgetvalue(a, ai, 2), PQgetvalue(b, bi, 2)) != 0) return false;
    if (pg_bool_value(PQgetvalue(a, ai, 3)) != pg_bool_value(PQgetvalue(b, bi, 3))) return false;
    {
        bool an = PQgetisnull(a, ai, 4);
        bool bn = PQgetisnull(b, bi, 4);
        if (an != bn) return false;
        if (!an && strcmp(PQgetvalue(a, ai, 4), PQgetvalue(b, bi, 4)) != 0) return false;
    }
    return true;
}

static void
emit_only_in_table(StringInfo out, PGresult *res, int s, int e, const char *json_key)
{
    bool first_col = true;
    int  i;

    appendStringInfo(out, "\"%s\":[", json_key);
    for (i = s; i < e; i++)
    {
        emit_column_object(out, res, i, first_col);
        first_col = false;
    }
    appendStringInfoChar(out, ']');
}

/*
 * Compare two column ranges (one table on each side). Walk-merge by
 * column name. Append a "modified" table entry if any drift is found.
 * Returns true when the entry was appended.
 */
static bool
emit_modified_table(StringInfo out, const char *tbl,
                    PGresult *src, int s_a, int s_b,
                    PGresult *tgt, int t_a, int t_b,
                    bool first_modified)
{
    /* First collect into a sub-buffer so we can decide whether to emit. */
    StringInfoData only_src, only_tgt, drift;
    int  i = s_a, j = t_a;
    bool any_diff = false;
    bool first_only_src = true, first_only_tgt = true, first_drift = true;
    int  cnt_only_src = 0, cnt_only_tgt = 0, cnt_drift = 0;

    initStringInfo(&only_src);
    initStringInfo(&only_tgt);
    initStringInfo(&drift);

    while (i < s_b || j < t_b)
    {
        const char *sn = (i < s_b) ? PQgetvalue(src, i, 1) : NULL;
        const char *tn = (j < t_b) ? PQgetvalue(tgt, j, 1) : NULL;
        int         cmp;

        if (sn == NULL)       cmp =  1;
        else if (tn == NULL)  cmp = -1;
        else                  cmp = strcmp(sn, tn);

        if (cmp < 0)
        {
            emit_column_object(&only_src, src, i, first_only_src);
            first_only_src = false; cnt_only_src++; any_diff = true;
            i++;
        }
        else if (cmp > 0)
        {
            emit_column_object(&only_tgt, tgt, j, first_only_tgt);
            first_only_tgt = false; cnt_only_tgt++; any_diff = true;
            j++;
        }
        else
        {
            if (!column_attrs_equal(src, i, tgt, j))
            {
                emit_drift_object(&drift, src, i, tgt, j, first_drift);
                first_drift = false; cnt_drift++; any_diff = true;
            }
            i++; j++;
        }
    }

    if (any_diff)
    {
        if (!first_modified)
            appendStringInfoChar(out, ',');
        appendStringInfoChar(out, '{');
        emit_json_pair_str(out, "name", tbl, true);
        appendStringInfo(out, ",\"columns_only_in_source\":[%s]",
                         cnt_only_src ? only_src.data : "");
        appendStringInfo(out, ",\"columns_only_in_target\":[%s]",
                         cnt_only_tgt ? only_tgt.data : "");
        appendStringInfo(out, ",\"columns_drift\":[%s]",
                         cnt_drift    ? drift.data    : "");
        appendStringInfoChar(out, '}');
    }

    pfree(only_src.data);
    pfree(only_tgt.data);
    pfree(drift.data);
    return any_diff;
}

static void
diff_tables_and_columns(PGconn *src_conn, PGconn *tgt_conn, const char *schema,
                        StringInfo out, int *out_diff_count,
                        int *only_src, int *only_tgt, int *modified)
{
    StringInfoData q;
    PGresult *src_res;
    PGresult *tgt_res;
    int  i = 0, j = 0;
    int  ns, nt;
    bool first_src = true, first_tgt = true, first_mod = true;
    StringInfoData buf_src, buf_tgt, buf_mod;

    build_columns_query(&q, schema);

    src_res = pgclone_diff_select(src_conn, q.data);
    tgt_res = pgclone_diff_select(tgt_conn, q.data);
    pfree(q.data);

    ns = PQntuples(src_res);
    nt = PQntuples(tgt_res);

    initStringInfo(&buf_src);
    initStringInfo(&buf_tgt);
    initStringInfo(&buf_mod);

    while (i < ns || j < nt)
    {
        const char *st = (i < ns) ? PQgetvalue(src_res, i, 0) : NULL;
        const char *tt = (j < nt) ? PQgetvalue(tgt_res, j, 0) : NULL;
        int         cmp;

        if (st == NULL)       cmp =  1;
        else if (tt == NULL)  cmp = -1;
        else                  cmp = strcmp(st, tt);

        if (cmp < 0)
        {
            int e = range_end(src_res, i);
            if (!first_src) appendStringInfoChar(&buf_src, ',');
            emit_json_str(&buf_src, st);
            first_src = false; (*only_src)++; (*out_diff_count)++;
            i = e;
        }
        else if (cmp > 0)
        {
            int e = range_end(tgt_res, j);
            if (!first_tgt) appendStringInfoChar(&buf_tgt, ',');
            emit_json_str(&buf_tgt, tt);
            first_tgt = false; (*only_tgt)++; (*out_diff_count)++;
            j = e;
        }
        else
        {
            int s_b = range_end(src_res, i);
            int t_b = range_end(tgt_res, j);
            if (emit_modified_table(&buf_mod, st, src_res, i, s_b, tgt_res, j, t_b, first_mod))
            {
                first_mod = false; (*modified)++; (*out_diff_count)++;
            }
            i = s_b;
            j = t_b;
        }
    }

    appendStringInfoString(out, "\"tables\":{");
    appendStringInfo(out, "\"only_in_source\":[%s]", buf_src.data);
    appendStringInfo(out, ",\"only_in_target\":[%s]", buf_tgt.data);
    appendStringInfo(out, ",\"modified\":[%s]", buf_mod.data);
    appendStringInfoChar(out, '}');

    pfree(buf_src.data);
    pfree(buf_tgt.data);
    pfree(buf_mod.data);
    PQclear(src_res);
    PQclear(tgt_res);
}

/* ===============================================================
 * Generic single-key + def diff used for indexes / constraints /
 * triggers / views.
 *
 * Caller passes a query producing rows with a STABLE sort order on
 * column 0 (the key) and column 1 holding the canonical definition
 * (e.g. pg_get_indexdef). Optional column 2 may contain a parent
 * table name for human-readable output.
 *
 * The query string is identical for source and target.
 * =============================================================== */
static void
diff_named_objects(PGconn *src_conn, PGconn *tgt_conn,
                   const char *category_label, const char *query,
                   StringInfo out, int *out_diff_count,
                   int *only_src, int *only_tgt, int *modified)
{
    PGresult *src_res = pgclone_diff_select(src_conn, query);
    PGresult *tgt_res = pgclone_diff_select(tgt_conn, query);
    int  ns = PQntuples(src_res);
    int  nt = PQntuples(tgt_res);
    int  i = 0, j = 0;
    bool first_src = true, first_tgt = true, first_mod = true;
    bool has_parent = (PQnfields(src_res) >= 3);
    StringInfoData buf_src, buf_tgt, buf_mod;

    initStringInfo(&buf_src);
    initStringInfo(&buf_tgt);
    initStringInfo(&buf_mod);

    while (i < ns || j < nt)
    {
        const char *sk = (i < ns) ? PQgetvalue(src_res, i, 0) : NULL;
        const char *tk = (j < nt) ? PQgetvalue(tgt_res, j, 0) : NULL;
        int cmp;

        if (sk == NULL)       cmp =  1;
        else if (tk == NULL)  cmp = -1;
        else                  cmp = strcmp(sk, tk);

        if (cmp < 0)
        {
            if (!first_src) appendStringInfoChar(&buf_src, ',');
            appendStringInfoChar(&buf_src, '{');
            emit_json_pair_str(&buf_src, "name", sk, true);
            if (has_parent && !PQgetisnull(src_res, i, 2))
                emit_json_pair_str(&buf_src, "table", PQgetvalue(src_res, i, 2), false);
            emit_json_pair_str(&buf_src, "def", PQgetvalue(src_res, i, 1), false);
            appendStringInfoChar(&buf_src, '}');
            first_src = false; (*only_src)++; (*out_diff_count)++;
            i++;
        }
        else if (cmp > 0)
        {
            if (!first_tgt) appendStringInfoChar(&buf_tgt, ',');
            appendStringInfoChar(&buf_tgt, '{');
            emit_json_pair_str(&buf_tgt, "name", tk, true);
            if (has_parent && !PQgetisnull(tgt_res, j, 2))
                emit_json_pair_str(&buf_tgt, "table", PQgetvalue(tgt_res, j, 2), false);
            emit_json_pair_str(&buf_tgt, "def", PQgetvalue(tgt_res, j, 1), false);
            appendStringInfoChar(&buf_tgt, '}');
            first_tgt = false; (*only_tgt)++; (*out_diff_count)++;
            j++;
        }
        else
        {
            const char *sd = PQgetvalue(src_res, i, 1);
            const char *td = PQgetvalue(tgt_res, j, 1);
            if (strcmp(sd, td) != 0)
            {
                if (!first_mod) appendStringInfoChar(&buf_mod, ',');
                appendStringInfoChar(&buf_mod, '{');
                emit_json_pair_str(&buf_mod, "name", sk, true);
                if (has_parent && !PQgetisnull(src_res, i, 2))
                    emit_json_pair_str(&buf_mod, "table", PQgetvalue(src_res, i, 2), false);
                emit_json_pair_str(&buf_mod, "source_def", sd, false);
                emit_json_pair_str(&buf_mod, "target_def", td, false);
                appendStringInfoChar(&buf_mod, '}');
                first_mod = false; (*modified)++; (*out_diff_count)++;
            }
            i++; j++;
        }
    }

    appendStringInfo(out, "\"%s\":{", category_label);
    appendStringInfo(out, "\"only_in_source\":[%s]", buf_src.data);
    appendStringInfo(out, ",\"only_in_target\":[%s]", buf_tgt.data);
    appendStringInfo(out, ",\"modified\":[%s]", buf_mod.data);
    appendStringInfoChar(out, '}');

    pfree(buf_src.data);
    pfree(buf_tgt.data);
    pfree(buf_mod.data);
    PQclear(src_res);
    PQclear(tgt_res);
}

/* ===============================================================
 * Sequences — single-key only (no def comparison in v4.1.0; the
 * sequence name itself is the unit of drift).
 * =============================================================== */
static void
diff_sequences(PGconn *src_conn, PGconn *tgt_conn, const char *schema,
               StringInfo out, int *out_diff_count,
               int *only_src, int *only_tgt)
{
    StringInfoData q;
    PGresult *src_res;
    PGresult *tgt_res;
    int  ns, nt, i = 0, j = 0;
    bool first_src = true, first_tgt = true;
    StringInfoData buf_src, buf_tgt;

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT c.relname FROM pg_catalog.pg_class c "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relkind = 'S' "
        "ORDER BY c.relname COLLATE \"C\"",
        quote_literal_cstr(schema));

    src_res = pgclone_diff_select(src_conn, q.data);
    tgt_res = pgclone_diff_select(tgt_conn, q.data);
    pfree(q.data);

    ns = PQntuples(src_res);
    nt = PQntuples(tgt_res);

    initStringInfo(&buf_src);
    initStringInfo(&buf_tgt);

    while (i < ns || j < nt)
    {
        const char *sk = (i < ns) ? PQgetvalue(src_res, i, 0) : NULL;
        const char *tk = (j < nt) ? PQgetvalue(tgt_res, j, 0) : NULL;
        int cmp;
        if (sk == NULL)       cmp =  1;
        else if (tk == NULL)  cmp = -1;
        else                  cmp = strcmp(sk, tk);

        if (cmp < 0)
        {
            if (!first_src) appendStringInfoChar(&buf_src, ',');
            emit_json_str(&buf_src, sk);
            first_src = false; (*only_src)++; (*out_diff_count)++;
            i++;
        }
        else if (cmp > 0)
        {
            if (!first_tgt) appendStringInfoChar(&buf_tgt, ',');
            emit_json_str(&buf_tgt, tk);
            first_tgt = false; (*only_tgt)++; (*out_diff_count)++;
            j++;
        }
        else { i++; j++; }
    }

    appendStringInfoString(out, "\"sequences\":{");
    appendStringInfo(out, "\"only_in_source\":[%s]", buf_src.data);
    appendStringInfo(out, ",\"only_in_target\":[%s]", buf_tgt.data);
    appendStringInfoChar(out, '}');

    pfree(buf_src.data);
    pfree(buf_tgt.data);
    PQclear(src_res);
    PQclear(tgt_res);
}

/* ===============================================================
 * FUNCTION: pgclone_diff(source_conninfo, schema_name) RETURNS TEXT
 *
 * Read-only. Returns a JSON document describing DDL drift between
 * the named schema on source and the same-named schema on the local
 * (target) database.
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_diff);

Datum
pgclone_diff(PG_FUNCTION_ARGS)
{
    text   *src_conninfo_t = PG_GETARG_TEXT_PP(0);
    text   *schema_t       = PG_GETARG_TEXT_PP(1);
    char   *src_conninfo   = text_to_cstring(src_conninfo_t);
    char   *schema         = text_to_cstring(schema_t);

    PGconn         *src = NULL;
    PGconn         *tgt = NULL;
    StringInfoData  out;
    StringInfoData  body;
    int diff_count = 0;
    int t_only_src = 0, t_only_tgt = 0, t_modified = 0;
    int i_only_src = 0, i_only_tgt = 0, i_modified = 0;
    int c_only_src = 0, c_only_tgt = 0, c_modified = 0;
    int g_only_src = 0, g_only_tgt = 0, g_modified = 0;
    int v_only_src = 0, v_only_tgt = 0, v_modified = 0;
    int s_only_src = 0, s_only_tgt = 0;

    pgclone_diff_validate_identifier(schema, "schema_name");

    initStringInfo(&out);
    initStringInfo(&body);

    PG_TRY();
    {
        StringInfoData q;

        src = pgclone_diff_connect_source(src_conninfo);
        tgt = pgclone_diff_connect_local();

        /* Tables + per-table column drift ------------------------- */
        diff_tables_and_columns(src, tgt, schema, &body, &diff_count,
                                &t_only_src, &t_only_tgt, &t_modified);

        /* Indexes (excluding those backing constraints) ----------- */
        initStringInfo(&q);
        appendStringInfo(&q,
            "SELECT i.relname, pg_get_indexdef(idx.indexrelid), c.relname "
            "FROM pg_catalog.pg_index idx "
            "JOIN pg_catalog.pg_class i ON i.oid = idx.indexrelid "
            "JOIN pg_catalog.pg_class c ON c.oid = idx.indrelid "
            "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
            "WHERE n.nspname = %s "
            "AND   c.relkind IN ('r','p','m') "
            "AND   NOT EXISTS (SELECT 1 FROM pg_catalog.pg_constraint con "
            "                  WHERE con.conindid = idx.indexrelid) "
            "ORDER BY i.relname COLLATE \"C\"",
            quote_literal_cstr(schema));
        appendStringInfoChar(&body, ',');
        diff_named_objects(src, tgt, "indexes", q.data, &body, &diff_count,
                           &i_only_src, &i_only_tgt, &i_modified);
        pfree(q.data);

        /* Constraints --------------------------------------------- */
        initStringInfo(&q);
        appendStringInfo(&q,
            "SELECT con.conname, pg_get_constraintdef(con.oid), cl.relname "
            "FROM pg_catalog.pg_constraint con "
            "JOIN pg_catalog.pg_class cl ON cl.oid = con.conrelid "
            "JOIN pg_catalog.pg_namespace n ON n.oid = cl.relnamespace "
            "WHERE n.nspname = %s "
            "ORDER BY con.conname COLLATE \"C\"",
            quote_literal_cstr(schema));
        appendStringInfoChar(&body, ',');
        diff_named_objects(src, tgt, "constraints", q.data, &body, &diff_count,
                           &c_only_src, &c_only_tgt, &c_modified);
        pfree(q.data);

        /* Triggers (user-defined only) ---------------------------- */
        initStringInfo(&q);
        appendStringInfo(&q,
            "SELECT tg.tgname, pg_get_triggerdef(tg.oid), cl.relname "
            "FROM pg_catalog.pg_trigger tg "
            "JOIN pg_catalog.pg_class cl ON cl.oid = tg.tgrelid "
            "JOIN pg_catalog.pg_namespace n ON n.oid = cl.relnamespace "
            "WHERE n.nspname = %s AND NOT tg.tgisinternal "
            "ORDER BY tg.tgname COLLATE \"C\"",
            quote_literal_cstr(schema));
        appendStringInfoChar(&body, ',');
        diff_named_objects(src, tgt, "triggers", q.data, &body, &diff_count,
                           &g_only_src, &g_only_tgt, &g_modified);
        pfree(q.data);

        /* Views + materialized views ------------------------------ */
        initStringInfo(&q);
        appendStringInfo(&q,
            "SELECT c.relname, pg_get_viewdef(c.oid, true) "
            "FROM pg_catalog.pg_class c "
            "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
            "WHERE n.nspname = %s AND c.relkind IN ('v','m') "
            "ORDER BY c.relname COLLATE \"C\"",
            quote_literal_cstr(schema));
        appendStringInfoChar(&body, ',');
        diff_named_objects(src, tgt, "views", q.data, &body, &diff_count,
                           &v_only_src, &v_only_tgt, &v_modified);
        pfree(q.data);

        /* Sequences ----------------------------------------------- */
        appendStringInfoChar(&body, ',');
        diff_sequences(src, tgt, schema, &body, &diff_count,
                       &s_only_src, &s_only_tgt);

        pgclone_diff_rollback(src);
        pgclone_diff_rollback(tgt);
        PQfinish(src); src = NULL;
        PQfinish(tgt); tgt = NULL;
    }
    PG_CATCH();
    {
        if (src) { pgclone_diff_rollback(src); PQfinish(src); }
        if (tgt) { pgclone_diff_rollback(tgt); PQfinish(tgt); }
        PG_RE_THROW();
    }
    PG_END_TRY();

    /* Assemble the final document. ------------------------------- */
    appendStringInfoChar(&out, '{');
    emit_json_pair_str (&out, "schema",    schema, true);
    emit_json_pair_bool(&out, "in_sync",   diff_count == 0, false);
    emit_json_pair_int (&out, "diff_count", diff_count, false);
    appendStringInfoString(&out, ",\"summary\":{");
    appendStringInfo(&out,
        "\"tables_only_in_source\":%d,\"tables_only_in_target\":%d,\"tables_modified\":%d,"
        "\"indexes_only_in_source\":%d,\"indexes_only_in_target\":%d,\"indexes_modified\":%d,"
        "\"constraints_only_in_source\":%d,\"constraints_only_in_target\":%d,\"constraints_modified\":%d,"
        "\"triggers_only_in_source\":%d,\"triggers_only_in_target\":%d,\"triggers_modified\":%d,"
        "\"views_only_in_source\":%d,\"views_only_in_target\":%d,\"views_modified\":%d,"
        "\"sequences_only_in_source\":%d,\"sequences_only_in_target\":%d",
        t_only_src, t_only_tgt, t_modified,
        i_only_src, i_only_tgt, i_modified,
        c_only_src, c_only_tgt, c_modified,
        g_only_src, g_only_tgt, g_modified,
        v_only_src, v_only_tgt, v_modified,
        s_only_src, s_only_tgt);
    appendStringInfoChar(&out, '}');
    appendStringInfoChar(&out, ',');
    appendStringInfoString(&out, body.data);
    appendStringInfoChar(&out, '}');

    pfree(body.data);
    PG_RETURN_TEXT_P(cstring_to_text(out.data));
}
