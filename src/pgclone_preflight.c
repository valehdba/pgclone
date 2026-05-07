/*
 * pgclone_preflight.c — Pre-flight validator for clone operations.
 *
 * Implements pgclone.preflight(source_conninfo, schema_name): a
 * read-only sanity check run before a clone, surfacing connection,
 * permission, version, capacity, and naming issues that would
 * otherwise fail mid-clone.
 *
 * Output is a JSON document with three top-level role-based arrays
 * (errors / warnings / info) plus a per-check details object.
 * `ready` is true when zero errors were recorded.
 *
 * Both source and local connections run inside REPEATABLE READ
 * READ ONLY transactions; this function never executes DDL or DML.
 *
 * Copyright (c) 2026, Valeh Agayev pgclone contributors
 * Licensed under PostgreSQL License
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "commands/dbcommands.h"

/* ===============================================================
 * Connection / query helpers (self-contained — does not share
 * symbols with pgclone.c or pgclone_diff.c).
 * =============================================================== */

static void
pf_normalize_session(PGconn *conn)
{
    PGresult *res = PQexec(conn, "SET search_path = pg_catalog");
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
        ereport(WARNING,
                (errmsg("pgclone.preflight: could not set search_path: %s",
                        PQerrorMessage(conn))));
    PQclear(res);
}

static void
pf_begin_readonly(PGconn *conn)
{
    PGresult *res = PQexec(conn,
        "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        char *msg = pstrdup(PQerrorMessage(conn));
        PQclear(res);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone.preflight: could not begin read-only transaction: %s", msg)));
    }
    PQclear(res);
}

static void
pf_rollback(PGconn *conn)
{
    PGresult *res = PQexec(conn, "ROLLBACK");
    if (res)
        PQclear(res);
}

static PGconn *
pf_connect_source(const char *conninfo)
{
    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK)
    {
        char *msg = pstrdup(PQerrorMessage(conn));
        PQfinish(conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgclone.preflight: could not connect to source: %s", msg)));
    }
    pf_normalize_session(conn);
    pf_begin_readonly(conn);
    return conn;
}

static void
pf_append_local_host(StringInfo conninfo)
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
pf_connect_local(void)
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
    pf_append_local_host(&conninfo);
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
                 errmsg("pgclone.preflight: could not connect to local database: %s", msg)));
    }
    pf_normalize_session(conn);
    pf_begin_readonly(conn);
    return conn;
}

static PGresult *
pf_select(PGconn *conn, const char *query)
{
    PGresult *res = PQexec(conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        char *msg = pstrdup(PQerrorMessage(conn));
        PQclear(res);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone.preflight: catalog query failed: %s", msg)));
    }
    return res;
}

/* ---------------------------------------------------------------
 * Identifier validator (defense in depth).
 * --------------------------------------------------------------- */
static void
pf_validate_identifier(const char *ident, const char *what)
{
    size_t len, i;

    if (ident == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone.preflight: %s must not be NULL", what)));

    len = strlen(ident);
    if (len == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone.preflight: %s must not be empty", what)));
    if (len >= NAMEDATALEN)
        ereport(ERROR,
                (errcode(ERRCODE_NAME_TOO_LONG),
                 errmsg("pgclone.preflight: %s exceeds %d bytes",
                        what, NAMEDATALEN - 1)));

    for (i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char) ident[i];
        if (c < 0x20 || c == 0x7F)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("pgclone.preflight: %s contains a control character", what)));
    }
}

/* ---------------------------------------------------------------
 * Local JSON string escaper. Identical to the one in
 * pgclone_diff.c — kept here so this translation unit has zero
 * dependencies on the rest of the codebase.
 * --------------------------------------------------------------- */
static void
pf_escape_json(StringInfo out, const char *s)
{
    const char *p;

    appendStringInfoChar(out, '"');
    for (p = s; *p != '\0'; p++)
    {
        unsigned char c = (unsigned char) *p;
        switch (c)
        {
            case '"':  appendStringInfoString(out, "\\\""); break;
            case '\\': appendStringInfoString(out, "\\\\"); break;
            case '\b': appendStringInfoString(out, "\\b");  break;
            case '\f': appendStringInfoString(out, "\\f");  break;
            case '\n': appendStringInfoString(out, "\\n");  break;
            case '\r': appendStringInfoString(out, "\\r");  break;
            case '\t': appendStringInfoString(out, "\\t");  break;
            default:
                if (c < 0x20)
                    appendStringInfo(out, "\\u%04x", c);
                else
                    appendStringInfoChar(out, (char) c);
                break;
        }
    }
    appendStringInfoChar(out, '"');
}

/* ===============================================================
 * Preflight context — accumulates the JSON document as checks run.
 * =============================================================== */
#define PF_PASS  "pass"
#define PF_FAIL  "fail"
#define PF_WARN  "warn"
#define PF_INFO  "info"
#define PF_SKIP  "skip"

typedef struct PreflightCtx
{
    StringInfoData checks;     /* "checks" object body, comma-separated */
    StringInfoData errors;     /* "errors" array body */
    StringInfoData warnings;   /* "warnings" array body */
    StringInfoData info;       /* "info" array body */
    int n_checks;
    int n_errors;
    int n_warnings;
    int n_info;
} PreflightCtx;

static void
pf_ctx_init(PreflightCtx *ctx)
{
    initStringInfo(&ctx->checks);
    initStringInfo(&ctx->errors);
    initStringInfo(&ctx->warnings);
    initStringInfo(&ctx->info);
    ctx->n_checks = 0;
    ctx->n_errors = 0;
    ctx->n_warnings = 0;
    ctx->n_info = 0;
}

static void
pf_ctx_free(PreflightCtx *ctx)
{
    pfree(ctx->checks.data);
    pfree(ctx->errors.data);
    pfree(ctx->warnings.data);
    pfree(ctx->info.data);
}

/*
 * Record a check. Prefix the JSON object with the check name as key.
 * Optionally appends `message` to the role-based summary buffer
 * (errors / warnings / info) when the status is fail / warn / info.
 *
 * `details_json` (may be NULL) is inlined verbatim; when supplied,
 * caller is responsible for it being well-formed JSON pairs without
 * surrounding braces (e.g. `"value":"16.3","major":16`).
 */
static void
pf_record(PreflightCtx *ctx, const char *name, const char *status,
          const char *message, const char *details_json)
{
    if (ctx->n_checks > 0)
        appendStringInfoChar(&ctx->checks, ',');
    appendStringInfo(&ctx->checks, "\"%s\":{\"status\":\"%s\"", name, status);
    if (message != NULL)
    {
        appendStringInfoString(&ctx->checks, ",\"message\":");
        pf_escape_json(&ctx->checks, message);
    }
    if (details_json != NULL && details_json[0] != '\0')
        appendStringInfo(&ctx->checks, ",%s", details_json);
    appendStringInfoChar(&ctx->checks, '}');
    ctx->n_checks++;

    if (message == NULL)
        return;

    if (strcmp(status, PF_FAIL) == 0)
    {
        if (ctx->n_errors > 0) appendStringInfoChar(&ctx->errors, ',');
        pf_escape_json(&ctx->errors, message);
        ctx->n_errors++;
    }
    else if (strcmp(status, PF_WARN) == 0)
    {
        if (ctx->n_warnings > 0) appendStringInfoChar(&ctx->warnings, ',');
        pf_escape_json(&ctx->warnings, message);
        ctx->n_warnings++;
    }
    else if (strcmp(status, PF_INFO) == 0)
    {
        if (ctx->n_info > 0) appendStringInfoChar(&ctx->info, ',');
        pf_escape_json(&ctx->info, message);
        ctx->n_info++;
    }
}

/* ===============================================================
 * Individual checks. Each takes a shared context plus the schema
 * name; queries source and/or target connections; records exactly
 * one entry into the context.
 * =============================================================== */

/* Returns true iff the column 0 of row 0 of `res` is t (PG bool). */
static bool
pf_bool_value(PGresult *res, int row, int col)
{
    if (PQntuples(res) <= row || PQgetisnull(res, row, col))
        return false;
    return PQgetvalue(res, row, col)[0] == 't';
}

/* ----- versions --------------------------------------------------- */
static int
fetch_server_version_num(PGconn *conn)
{
    PGresult *res = pf_select(conn, "SELECT current_setting('server_version_num')");
    int n = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    return n;
}

static char *
fetch_server_version_string(PGconn *conn)
{
    PGresult *res = pf_select(conn, "SELECT current_setting('server_version')");
    char *s = pstrdup(PQgetvalue(res, 0, 0));
    PQclear(res);
    return s;
}

static void
check_versions(PreflightCtx *ctx, PGconn *src, PGconn *tgt)
{
    int   src_num = fetch_server_version_num(src);
    int   tgt_num = fetch_server_version_num(tgt);
    char *src_str = fetch_server_version_string(src);
    char *tgt_str = fetch_server_version_string(tgt);
    int   src_major = src_num / 10000;
    int   tgt_major = tgt_num / 10000;
    StringInfoData details;

    initStringInfo(&details);
    appendStringInfo(&details, "\"value\":");
    pf_escape_json(&details, src_str);
    appendStringInfo(&details, ",\"major\":%d", src_major);
    pf_record(ctx, "source_version", PF_INFO, NULL, details.data);
    pfree(details.data);

    initStringInfo(&details);
    appendStringInfo(&details, "\"value\":");
    pf_escape_json(&details, tgt_str);
    appendStringInfo(&details, ",\"major\":%d", tgt_major);
    pf_record(ctx, "target_version", PF_INFO, NULL, details.data);
    pfree(details.data);

    if (src_major > tgt_major)
    {
        StringInfoData m;
        initStringInfo(&m);
        appendStringInfo(&m,
            "source major version %d is higher than target %d — clone may fail "
            "on features unsupported by the older target",
            src_major, tgt_major);
        pf_record(ctx, "version_compat", PF_WARN, m.data, NULL);
        pfree(m.data);
    }
    else
    {
        pf_record(ctx, "version_compat", PF_PASS, NULL, NULL);
    }

    pfree(src_str);
    pfree(tgt_str);
}

/* ----- schema existence ----------------------------------------- */
static bool
schema_exists(PGconn *conn, const char *schema)
{
    StringInfoData q;
    PGresult *res;
    bool exists;

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_namespace WHERE nspname = %s)",
        quote_literal_cstr(schema));
    res = pf_select(conn, q.data);
    pfree(q.data);
    exists = pf_bool_value(res, 0, 0);
    PQclear(res);
    return exists;
}

static void
check_schema_existence(PreflightCtx *ctx, PGconn *src, PGconn *tgt,
                       const char *schema, bool *out_target_has_schema)
{
    bool src_has = schema_exists(src, schema);
    bool tgt_has = schema_exists(tgt, schema);

    if (!src_has)
    {
        StringInfoData m;
        initStringInfo(&m);
        appendStringInfo(&m, "schema \"%s\" does not exist on source", schema);
        pf_record(ctx, "schema_exists_source", PF_FAIL, m.data, NULL);
        pfree(m.data);
    }
    else
    {
        pf_record(ctx, "schema_exists_source", PF_PASS, NULL, NULL);
    }

    if (tgt_has)
    {
        pf_record(ctx, "schema_exists_target", PF_INFO, NULL, "\"value\":true");
    }
    else
    {
        StringInfoData m;
        initStringInfo(&m);
        appendStringInfo(&m, "target schema \"%s\" does not exist — pgclone will create it", schema);
        pf_record(ctx, "schema_exists_target", PF_INFO, m.data, "\"value\":false");
        pfree(m.data);
    }

    *out_target_has_schema = tgt_has;
}

/* ----- permissions ---------------------------------------------- */
static bool
fetch_bool_query(PGconn *conn, const char *query)
{
    PGresult *res = pf_select(conn, query);
    bool b = pf_bool_value(res, 0, 0);
    PQclear(res);
    return b;
}

static void
check_source_permissions(PreflightCtx *ctx, PGconn *src, const char *schema)
{
    StringInfoData q;
    bool has_usage;

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT has_schema_privilege(current_user, %s, 'USAGE')",
        quote_literal_cstr(schema));
    has_usage = fetch_bool_query(src, q.data);
    pfree(q.data);

    if (!has_usage)
    {
        StringInfoData m;
        initStringInfo(&m);
        appendStringInfo(&m,
            "source role lacks USAGE on schema \"%s\"; grant: "
            "GRANT USAGE ON SCHEMA %s TO <role>",
            schema, schema);
        pf_record(ctx, "source_permissions", PF_FAIL, m.data, NULL);
        pfree(m.data);
        return;
    }

    /*
     * Test SELECT on every table+view in the schema. If any one fails we
     * emit a warning naming the first offender. (A clone of the whole
     * schema requires SELECT on every relation.)
     */
    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT c.relname FROM pg_catalog.pg_class c "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relkind IN ('r','p','v','m') "
        "  AND NOT has_table_privilege(current_user, c.oid, 'SELECT') "
        "ORDER BY c.relname COLLATE \"C\" LIMIT 5",
        quote_literal_cstr(schema));
    {
        PGresult *res = pf_select(src, q.data);
        int n = PQntuples(res);
        if (n == 0)
        {
            pf_record(ctx, "source_permissions", PF_PASS, NULL, NULL);
        }
        else
        {
            StringInfoData m;
            int i;
            initStringInfo(&m);
            appendStringInfoString(&m, "source role lacks SELECT on: ");
            for (i = 0; i < n; i++)
            {
                if (i) appendStringInfoString(&m, ", ");
                appendStringInfoString(&m, PQgetvalue(res, i, 0));
            }
            if (n == 5)
                appendStringInfoString(&m, " (and possibly more)");
            pf_record(ctx, "source_permissions", PF_FAIL, m.data, NULL);
            pfree(m.data);
        }
        PQclear(res);
    }
    pfree(q.data);
}

static void
check_target_permissions(PreflightCtx *ctx, PGconn *tgt, const char *schema,
                         bool target_has_schema)
{
    StringInfoData q;
    bool has_create;

    initStringInfo(&q);
    if (target_has_schema)
    {
        appendStringInfo(&q,
            "SELECT has_schema_privilege(current_user, %s, 'CREATE')",
            quote_literal_cstr(schema));
    }
    else
    {
        appendStringInfoString(&q,
            "SELECT has_database_privilege(current_user, current_database(), 'CREATE')");
    }
    has_create = fetch_bool_query(tgt, q.data);
    pfree(q.data);

    if (has_create)
    {
        pf_record(ctx, "target_permissions", PF_PASS, NULL, NULL);
    }
    else
    {
        StringInfoData m;
        initStringInfo(&m);
        if (target_has_schema)
            appendStringInfo(&m,
                "target role lacks CREATE on schema \"%s\"", schema);
        else
            appendStringInfoString(&m,
                "target role lacks CREATE on the current database (cannot create the missing schema)");
        pf_record(ctx, "target_permissions", PF_FAIL, m.data, NULL);
        pfree(m.data);
    }
}

/* ----- size + counts -------------------------------------------- */
static void
check_size_and_counts(PreflightCtx *ctx, PGconn *src, PGconn *tgt,
                      const char *schema)
{
    StringInfoData q;
    PGresult *res;
    int64  est_bytes = 0;
    int    n_tables = 0, n_views = 0, n_seqs = 0, n_idx = 0;
    int64  tgt_db_bytes = 0;

    /* source: estimated total relation size for tables + matviews */
    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT COALESCE(SUM(pg_total_relation_size(c.oid))::bigint, 0) "
        "FROM pg_catalog.pg_class c "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relkind IN ('r','p','m')",
        quote_literal_cstr(schema));
    res = pf_select(src, q.data);
    if (PQntuples(res) > 0)
        est_bytes = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    pfree(q.data);

    /* source: object counts */
    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT "
        " (SELECT count(*) FROM pg_catalog.pg_class c "
        "    JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "    WHERE n.nspname = %s AND c.relkind IN ('r','p')), "
        " (SELECT count(*) FROM pg_catalog.pg_class c "
        "    JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "    WHERE n.nspname = %s AND c.relkind IN ('v','m')), "
        " (SELECT count(*) FROM pg_catalog.pg_class c "
        "    JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "    WHERE n.nspname = %s AND c.relkind = 'S'), "
        " (SELECT count(*) FROM pg_catalog.pg_index i "
        "    JOIN pg_catalog.pg_class c ON c.oid = i.indrelid "
        "    JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "    WHERE n.nspname = %s)",
        quote_literal_cstr(schema),
        quote_literal_cstr(schema),
        quote_literal_cstr(schema),
        quote_literal_cstr(schema));
    res = pf_select(src, q.data);
    if (PQntuples(res) > 0)
    {
        n_tables = atoi(PQgetvalue(res, 0, 0));
        n_views  = atoi(PQgetvalue(res, 0, 1));
        n_seqs   = atoi(PQgetvalue(res, 0, 2));
        n_idx    = atoi(PQgetvalue(res, 0, 3));
    }
    PQclear(res);
    pfree(q.data);

    /* target: current database size */
    res = pf_select(tgt, "SELECT pg_database_size(current_database())::bigint");
    if (PQntuples(res) > 0)
        tgt_db_bytes = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);

    {
        StringInfoData details;
        initStringInfo(&details);
        appendStringInfo(&details, "\"bytes\":%lld", (long long) est_bytes);
        pf_record(ctx, "estimated_size", PF_INFO, NULL, details.data);
        pfree(details.data);
    }
    {
        StringInfoData details;
        initStringInfo(&details);
        appendStringInfo(&details, "\"bytes\":%lld", (long long) tgt_db_bytes);
        pf_record(ctx, "target_database_size", PF_INFO, NULL, details.data);
        pfree(details.data);
    }
    {
        StringInfoData details;
        initStringInfo(&details);
        appendStringInfo(&details,
            "\"tables\":%d,\"views\":%d,\"sequences\":%d,\"indexes\":%d",
            n_tables, n_views, n_seqs, n_idx);
        pf_record(ctx, "object_counts", PF_INFO, NULL, details.data);
        pfree(details.data);
    }
}

/* ----- name conflicts ------------------------------------------- */
static void
check_name_conflicts(PreflightCtx *ctx, PGconn *src, PGconn *tgt,
                     const char *schema, bool target_has_schema)
{
    StringInfoData q, items;
    PGresult *src_res, *tgt_res;
    int  i = 0, j = 0, ns, nt, n_conf = 0;

    if (!target_has_schema)
    {
        pf_record(ctx, "name_conflicts", PF_PASS, NULL, "\"items\":[]");
        return;
    }

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT c.relname, c.relkind FROM pg_catalog.pg_class c "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relkind IN ('r','p','v','m','S','f') "
        "ORDER BY c.relname COLLATE \"C\"",
        quote_literal_cstr(schema));

    src_res = pf_select(src, q.data);
    tgt_res = pf_select(tgt, q.data);
    pfree(q.data);

    initStringInfo(&items);
    appendStringInfoChar(&items, '[');
    ns = PQntuples(src_res);
    nt = PQntuples(tgt_res);

    while (i < ns && j < nt)
    {
        int cmp = strcmp(PQgetvalue(src_res, i, 0), PQgetvalue(tgt_res, j, 0));
        if (cmp == 0)
        {
            if (n_conf > 0) appendStringInfoChar(&items, ',');
            appendStringInfoChar(&items, '{');
            appendStringInfoString(&items, "\"name\":");
            pf_escape_json(&items, PQgetvalue(src_res, i, 0));
            appendStringInfo(&items, ",\"kind\":\"%s\"",
                             PQgetvalue(src_res, i, 1));
            appendStringInfoChar(&items, '}');
            n_conf++;
            i++; j++;
        }
        else if (cmp < 0) i++;
        else              j++;
    }

    appendStringInfoChar(&items, ']');

    PQclear(src_res);
    PQclear(tgt_res);

    if (n_conf == 0)
    {
        pf_record(ctx, "name_conflicts", PF_PASS, NULL, "\"items\":[]");
    }
    else
    {
        StringInfoData m, details;
        initStringInfo(&m);
        appendStringInfo(&m,
            "%d object name(s) already exist on target schema \"%s\"; "
            "use a conflict_resolution option (skip / replace / rename) when cloning",
            n_conf, schema);
        initStringInfo(&details);
        appendStringInfo(&details, "\"count\":%d,\"items\":%s", n_conf, items.data);
        pf_record(ctx, "name_conflicts", PF_WARN, m.data, details.data);
        pfree(m.data);
        pfree(details.data);
    }
    pfree(items.data);
}

/* ----- missing extensions --------------------------------------- */
static void
check_missing_extensions(PreflightCtx *ctx, PGconn *src, PGconn *tgt)
{
    PGresult *src_res = pf_select(src,
        "SELECT extname FROM pg_catalog.pg_extension "
        "WHERE extname <> 'plpgsql' "
        "ORDER BY extname COLLATE \"C\"");
    PGresult *tgt_res = pf_select(tgt,
        "SELECT extname FROM pg_catalog.pg_extension "
        "ORDER BY extname COLLATE \"C\"");
    int   ns = PQntuples(src_res), nt = PQntuples(tgt_res);
    int   i = 0, j = 0;
    StringInfoData missing_csv;

    initStringInfo(&missing_csv);
    while (i < ns)
    {
        const char *s = PQgetvalue(src_res, i, 0);
        int cmp;
        if (j >= nt) cmp = -1;
        else cmp = strcmp(s, PQgetvalue(tgt_res, j, 0));
        if (cmp < 0)
        {
            if (missing_csv.len > 0) appendStringInfoChar(&missing_csv, '\n');
            appendStringInfoString(&missing_csv, s);
            i++;
        }
        else if (cmp == 0) { i++; j++; }
        else j++;
    }

    if (missing_csv.len == 0)
    {
        pf_record(ctx, "missing_extensions", PF_PASS, NULL, "\"items\":[]");
    }
    else
    {
        StringInfoData items, m;
        char *line;
        int n = 0;

        initStringInfo(&items);
        appendStringInfoChar(&items, '[');
        for (line = missing_csv.data; line && *line; )
        {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (n) appendStringInfoChar(&items, ',');
            pf_escape_json(&items, line);
            n++;
            line = nl ? nl + 1 : NULL;
        }
        appendStringInfoChar(&items, ']');

        initStringInfo(&m);
        appendStringInfo(&m,
            "%d extension(s) installed on source but not on target — "
            "DDL referencing their types/functions will fail to replay", n);
        {
            StringInfoData details;
            initStringInfo(&details);
            appendStringInfo(&details, "\"count\":%d,\"items\":%s", n, items.data);
            pf_record(ctx, "missing_extensions", PF_WARN, m.data, details.data);
            pfree(details.data);
        }
        pfree(items.data);
        pfree(m.data);
    }

    pfree(missing_csv.data);
    PQclear(src_res);
    PQclear(tgt_res);
}

/* ----- missing roles -------------------------------------------- */
static void
check_missing_roles(PreflightCtx *ctx, PGconn *src, PGconn *tgt,
                    const char *schema)
{
    StringInfoData q;
    PGresult *res;
    int  n;
    int  i;
    StringInfoData missing;

    /*
     * Collect distinct role names referenced by table OWNER and ACL
     * grantees in the target schema's source-side relations.
     */
    initStringInfo(&q);
    appendStringInfo(&q,
        "WITH rels AS ( "
        "  SELECT c.oid, c.relowner, c.relacl "
        "  FROM pg_catalog.pg_class c "
        "  JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "  WHERE n.nspname = %s AND c.relkind IN ('r','p','v','m','S','f') "
        "), "
        "owner_roles AS ( "
        "  SELECT pg_catalog.pg_get_userbyid(relowner) AS rolname FROM rels "
        "), "
        "acl_roles AS ( "
        "  SELECT (aclexplode(relacl)).grantee AS grantee_oid FROM rels "
        "  WHERE relacl IS NOT NULL "
        "), "
        "all_roles AS ( "
        "  SELECT rolname FROM owner_roles "
        "  UNION "
        "  SELECT pg_catalog.pg_get_userbyid(grantee_oid) FROM acl_roles "
        "  WHERE grantee_oid <> 0 "
        ") "
        "SELECT DISTINCT rolname FROM all_roles "
        "WHERE rolname IS NOT NULL "
        "ORDER BY rolname",
        quote_literal_cstr(schema));
    res = pf_select(src, q.data);
    pfree(q.data);

    initStringInfo(&missing);
    n = PQntuples(res);
    for (i = 0; i < n; i++)
    {
        const char *rolname = PQgetvalue(res, i, 0);
        StringInfoData chk;
        bool exists;
        initStringInfo(&chk);
        appendStringInfo(&chk,
            "SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_roles WHERE rolname = %s)",
            quote_literal_cstr(rolname));
        exists = fetch_bool_query(tgt, chk.data);
        pfree(chk.data);
        if (!exists)
        {
            if (missing.len > 0) appendStringInfoChar(&missing, '\n');
            appendStringInfoString(&missing, rolname);
        }
    }
    PQclear(res);

    if (missing.len == 0)
    {
        pf_record(ctx, "missing_roles", PF_PASS, NULL, "\"items\":[]");
    }
    else
    {
        StringInfoData items, m, details;
        char *line;
        int  k = 0;

        initStringInfo(&items);
        appendStringInfoChar(&items, '[');
        for (line = missing.data; line && *line; )
        {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (k) appendStringInfoChar(&items, ',');
            pf_escape_json(&items, line);
            k++;
            line = nl ? nl + 1 : NULL;
        }
        appendStringInfoChar(&items, ']');

        initStringInfo(&m);
        appendStringInfo(&m,
            "%d role(s) referenced by source schema do not exist on target — "
            "consider running pgclone.clone_roles() first", k);
        initStringInfo(&details);
        appendStringInfo(&details, "\"count\":%d,\"items\":%s", k, items.data);
        pf_record(ctx, "missing_roles", PF_WARN, m.data, details.data);
        pfree(items.data);
        pfree(m.data);
        pfree(details.data);
    }

    pfree(missing.data);
}

/* ----- missing tablespaces -------------------------------------- */
static void
check_missing_tablespaces(PreflightCtx *ctx, PGconn *src, PGconn *tgt,
                          const char *schema)
{
    StringInfoData q;
    PGresult *res;
    int  n, i;
    StringInfoData missing;

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT DISTINCT t.spcname "
        "FROM pg_catalog.pg_class c "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "JOIN pg_catalog.pg_tablespace t ON t.oid = c.reltablespace "
        "WHERE n.nspname = %s "
        "  AND c.reltablespace <> 0 "
        "  AND t.spcname NOT IN ('pg_default','pg_global') "
        "ORDER BY t.spcname",
        quote_literal_cstr(schema));
    res = pf_select(src, q.data);
    pfree(q.data);

    initStringInfo(&missing);
    n = PQntuples(res);
    for (i = 0; i < n; i++)
    {
        const char *ts = PQgetvalue(res, i, 0);
        StringInfoData chk;
        bool exists;
        initStringInfo(&chk);
        appendStringInfo(&chk,
            "SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_tablespace WHERE spcname = %s)",
            quote_literal_cstr(ts));
        exists = fetch_bool_query(tgt, chk.data);
        pfree(chk.data);
        if (!exists)
        {
            if (missing.len > 0) appendStringInfoChar(&missing, '\n');
            appendStringInfoString(&missing, ts);
        }
    }
    PQclear(res);

    if (missing.len == 0)
    {
        pf_record(ctx, "missing_tablespaces", PF_PASS, NULL, "\"items\":[]");
    }
    else
    {
        StringInfoData items, m, details;
        char *line;
        int  k = 0;

        initStringInfo(&items);
        appendStringInfoChar(&items, '[');
        for (line = missing.data; line && *line; )
        {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (k) appendStringInfoChar(&items, ',');
            pf_escape_json(&items, line);
            k++;
            line = nl ? nl + 1 : NULL;
        }
        appendStringInfoChar(&items, ']');

        initStringInfo(&m);
        appendStringInfo(&m,
            "%d non-default tablespace(s) referenced by source not present on target", k);
        initStringInfo(&details);
        appendStringInfo(&details, "\"count\":%d,\"items\":%s", k, items.data);
        pf_record(ctx, "missing_tablespaces", PF_WARN, m.data, details.data);
        pfree(items.data);
        pfree(m.data);
        pfree(details.data);
    }
    pfree(missing.data);
}

/* ===============================================================
 * FUNCTION: pgclone_preflight(source_conninfo, schema_name) RETURNS TEXT
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_preflight);

Datum
pgclone_preflight(PG_FUNCTION_ARGS)
{
    text   *src_conninfo_t = PG_GETARG_TEXT_PP(0);
    text   *schema_t       = PG_GETARG_TEXT_PP(1);
    char   *src_conninfo   = text_to_cstring(src_conninfo_t);
    char   *schema         = text_to_cstring(schema_t);

    PGconn        *src = NULL;
    PGconn        *tgt = NULL;
    PreflightCtx   ctx;
    StringInfoData out;
    bool           target_has_schema = false;

    pf_validate_identifier(schema, "schema_name");
    pf_ctx_init(&ctx);

    PG_TRY();
    {
        /* Connection checks. The connect helpers raise on failure;
         * if we get past these, both connections are healthy. */
        src = pf_connect_source(src_conninfo);
        pf_record(&ctx, "source_connection", PF_PASS, NULL, NULL);

        tgt = pf_connect_local();
        pf_record(&ctx, "target_connection", PF_PASS, NULL, NULL);

        check_versions(&ctx, src, tgt);
        check_schema_existence(&ctx, src, tgt, schema, &target_has_schema);

        /* Only run permission and content checks if source has the schema. */
        if (ctx.n_errors == 0)
        {
            check_source_permissions(&ctx, src, schema);
            check_target_permissions(&ctx, tgt, schema, target_has_schema);
            check_size_and_counts(&ctx, src, tgt, schema);
            check_name_conflicts(&ctx, src, tgt, schema, target_has_schema);
            check_missing_extensions(&ctx, src, tgt);
            check_missing_roles(&ctx, src, tgt, schema);
            check_missing_tablespaces(&ctx, src, tgt, schema);
        }

        pf_rollback(src); PQfinish(src); src = NULL;
        pf_rollback(tgt); PQfinish(tgt); tgt = NULL;
    }
    PG_CATCH();
    {
        if (src) { pf_rollback(src); PQfinish(src); }
        if (tgt) { pf_rollback(tgt); PQfinish(tgt); }
        pf_ctx_free(&ctx);
        PG_RE_THROW();
    }
    PG_END_TRY();

    /* Assemble final document. */
    initStringInfo(&out);
    appendStringInfoChar(&out, '{');
    appendStringInfoString(&out, "\"schema\":");
    pf_escape_json(&out, schema);
    appendStringInfo(&out, ",\"ready\":%s",
                     ctx.n_errors == 0 ? "true" : "false");
    appendStringInfo(&out,
        ",\"summary\":{\"errors\":%d,\"warnings\":%d,\"info\":%d,\"checks_run\":%d}",
        ctx.n_errors, ctx.n_warnings, ctx.n_info, ctx.n_checks);
    appendStringInfo(&out, ",\"errors\":[%s]",   ctx.errors.data);
    appendStringInfo(&out, ",\"warnings\":[%s]", ctx.warnings.data);
    appendStringInfo(&out, ",\"info\":[%s]",     ctx.info.data);
    appendStringInfo(&out, ",\"checks\":{%s}",   ctx.checks.data);
    appendStringInfoChar(&out, '}');

    pf_ctx_free(&ctx);

    PG_RETURN_TEXT_P(cstring_to_text(out.data));
}
