/*
 * pgx_clone - PostgreSQL extension for cloning databases, schemas, tables,
 *            and functions between PostgreSQL hosts.
 *
 * Copyright (c) 2026, pgx_clone contributors
 * Licensed under PostgreSQL License
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "executor/spi.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "catalog/pg_type.h"
#include "utils/syscache.h"
#include "utils/guc.h"
#include "commands/dbcommands.h"
#include "utils/jsonb.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

#include "pgx_clone_bgw.h"

PG_MODULE_MAGIC;

/* ---------------------------------------------------------------
 * Clone options struct — controls what gets cloned beyond the
 * table structure and data.
 * --------------------------------------------------------------- */
#define PGX_CLONE_MAX_COLUMNS   64
#define PGX_CLONE_MAX_WHERE     2048

typedef struct CloneOptions
{
    bool include_indexes;
    bool include_constraints;
    bool include_triggers;

    /* Selective column cloning: if num_columns > 0, only these columns */
    int  num_columns;
    char columns[PGX_CLONE_MAX_COLUMNS][NAMEDATALEN];

    /* Data filtering: WHERE clause applied during COPY */
    char where_clause[PGX_CLONE_MAX_WHERE];
} CloneOptions;

/* Default: everything enabled, no column/where filter */
static CloneOptions
pgx_clone_default_options(void)
{
    CloneOptions opts;
    memset(&opts, 0, sizeof(CloneOptions));
    opts.include_indexes     = true;
    opts.include_constraints = true;
    opts.include_triggers    = true;
    opts.num_columns         = 0;
    opts.where_clause[0]     = '\0';
    return opts;
}

/* ---------------------------------------------------------------
 * Parse a JSON options string like:
 *   {"indexes": false, "constraints": true, "triggers": false,
 *    "columns": ["id", "name", "email"],
 *    "where": "status = 'active' AND created_at > '2024-01-01'"}
 *
 * Missing keys keep defaults. Simple parser — no external deps.
 * --------------------------------------------------------------- */
static CloneOptions
pgx_clone_parse_options(const char *json_str)
{
    CloneOptions opts = pgx_clone_default_options();
    const char *p;

    if (json_str == NULL || json_str[0] == '\0')
        return opts;

    /* Boolean options */
    if (strstr(json_str, "\"indexes\": false") != NULL ||
        strstr(json_str, "\"indexes\":false") != NULL)
        opts.include_indexes = false;

    if (strstr(json_str, "\"constraints\": false") != NULL ||
        strstr(json_str, "\"constraints\":false") != NULL)
        opts.include_constraints = false;

    if (strstr(json_str, "\"triggers\": false") != NULL ||
        strstr(json_str, "\"triggers\":false") != NULL)
        opts.include_triggers = false;

    /* Parse "columns": ["col1", "col2", ...] */
    p = strstr(json_str, "\"columns\"");
    if (p != NULL)
    {
        const char *bracket = strchr(p, '[');
        if (bracket != NULL)
        {
            const char *end_bracket = strchr(bracket, ']');
            if (end_bracket != NULL)
            {
                const char *cur = bracket + 1;
                opts.num_columns = 0;

                while (cur < end_bracket && opts.num_columns < PGX_CLONE_MAX_COLUMNS)
                {
                    const char *quote_start, *quote_end;

                    /* Find opening quote */
                    quote_start = strchr(cur, '"');
                    if (!quote_start || quote_start >= end_bracket)
                        break;
                    quote_start++;

                    /* Find closing quote */
                    quote_end = strchr(quote_start, '"');
                    if (!quote_end || quote_end >= end_bracket)
                        break;

                    {
                        int len = quote_end - quote_start;
                        if (len > 0 && len < NAMEDATALEN)
                        {
                            memcpy(opts.columns[opts.num_columns], quote_start, len);
                            opts.columns[opts.num_columns][len] = '\0';
                            opts.num_columns++;
                        }
                    }

                    cur = quote_end + 1;
                }
            }
        }
    }

    /* Parse "where": "condition..." */
    p = strstr(json_str, "\"where\"");
    if (p == NULL)
        p = strstr(json_str, "\"filter\"");  /* accept "filter" as alias */

    if (p != NULL)
    {
        const char *colon = strchr(p, ':');
        if (colon != NULL)
        {
            const char *quote_start = strchr(colon, '"');
            if (quote_start != NULL)
            {
                const char *quote_end;
                quote_start++;

                /* Find end quote — handle escaped quotes */
                quote_end = quote_start;
                while (*quote_end != '\0')
                {
                    if (*quote_end == '\\' && *(quote_end + 1) == '"')
                    {
                        quote_end += 2;
                        continue;
                    }
                    if (*quote_end == '"')
                        break;
                    quote_end++;
                }

                {
                    int len = quote_end - quote_start;
                    if (len > 0 && len < PGX_CLONE_MAX_WHERE)
                    {
                        memcpy(opts.where_clause, quote_start, len);
                        opts.where_clause[len] = '\0';
                    }
                }
            }
        }
    }

    return opts;
}

/* ---------------------------------------------------------------
 * Internal helper: connect to a remote PostgreSQL host
 * --------------------------------------------------------------- */
static PGconn *
pgx_clone_connect(const char *conninfo)
{
    PGconn *conn;

    conn = PQconnectdb(conninfo);

    if (PQstatus(conn) != CONNECTION_OK)
    {
        char *conn_errmsg = pstrdup(PQerrorMessage(conn));
        PQfinish(conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgx_clone: could not connect to remote host: %s", conn_errmsg)));
    }

    return conn;
}

/* ---------------------------------------------------------------
 * Internal helper: execute a query on a remote connection
 * Returns the PGresult (caller must PQclear it)
 * --------------------------------------------------------------- */
static PGresult *
pgx_clone_exec(PGconn *conn, const char *query)
{
    PGresult *res;

    res = PQexec(conn, query);

    if (PQresultStatus(res) != PGRES_TUPLES_OK &&
        PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        char *exec_errmsg = pstrdup(PQerrorMessage(conn));
        PQclear(res);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgx_clone: remote query failed: %s", exec_errmsg)));
    }

    return res;
}

/* ---------------------------------------------------------------
 * Internal helper: execute DDL on the local database via SPI
 * --------------------------------------------------------------- */
static void
pgx_clone_exec_local(const char *query)
{
    int ret;

    ret = SPI_execute(query, false, 0);

    if (ret != SPI_OK_UTILITY && ret != SPI_OK_SELECT &&
        ret != SPI_OK_INSERT && ret != SPI_OK_UPDATE)
    {
        ereport(WARNING,
                (errmsg("pgx_clone: local execution returned code %d for: %.128s",
                        ret, query)));
    }
}

/* ---------------------------------------------------------------
 * Internal helper: execute DDL on a libpq connection.
 * Used for loopback connection operations.
 * Returns true on success, false on failure (with WARNING).
 * --------------------------------------------------------------- */
static bool
pgx_clone_exec_conn(PGconn *conn, const char *query)
{
    PGresult *res;

    res = PQexec(conn, query);

    if (PQresultStatus(res) != PGRES_COMMAND_OK &&
        PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        char *conn_errmsg = pstrdup(PQerrorMessage(conn));
        PQclear(res);
        ereport(WARNING,
                (errmsg("pgx_clone: local exec failed: %s (query: %.128s)",
                        conn_errmsg, query)));
        return false;
    }

    PQclear(res);
    return true;
}

/* ---------------------------------------------------------------
 * Internal helper: get a libpq connection to the LOCAL database.
 * --------------------------------------------------------------- */
static PGconn *
pgx_clone_connect_local(void)
{
    PGconn         *conn;
    StringInfoData  conninfo;
    const char     *dbname;
    const char     *port;

    dbname = get_database_name(MyDatabaseId);
    port = GetConfigOption("port", false, false);

    initStringInfo(&conninfo);
    appendStringInfo(&conninfo, "dbname=%s port=%s",
                     quote_literal_cstr(dbname),
                     port ? port : "5432");

    conn = PQconnectdb(conninfo.data);
    pfree(conninfo.data);

    if (PQstatus(conn) != CONNECTION_OK)
    {
        char *local_errmsg = pstrdup(PQerrorMessage(conn));
        PQfinish(conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgx_clone: could not connect to local database: %s",
                        local_errmsg)));
    }

    return conn;
}

/* ---------------------------------------------------------------
 * Internal helper: stream data from source to target using COPY.
 *
 * When columns or where_clause are provided, uses
 * COPY (SELECT cols FROM table WHERE filter) TO STDOUT
 * instead of COPY table TO STDOUT.
 * --------------------------------------------------------------- */
static int64
pgx_clone_copy_data(PGconn *source_conn, PGconn *local_conn,
                    const char *schema_name, const char *source_table,
                    const char *target_table, const CloneOptions *opts)
{
    PGresult       *res;
    StringInfoData  cmd;
    char           *buf;
    int             ret;
    int64           bytes_transferred = 0;
    int64           row_count = 0;
    bool            use_query_copy;

    /* Determine if we need query-based COPY (for columns/WHERE) */
    use_query_copy = (opts != NULL &&
                      (opts->num_columns > 0 || opts->where_clause[0] != '\0'));

    /* Start COPY OUT on source */
    initStringInfo(&cmd);

    if (use_query_copy)
    {
        /* COPY (SELECT columns FROM table WHERE filter) TO STDOUT */
        StringInfoData select_cmd;
        initStringInfo(&select_cmd);

        appendStringInfoString(&select_cmd, "SELECT ");

        if (opts->num_columns > 0)
        {
            int ci;
            for (ci = 0; ci < opts->num_columns; ci++)
            {
                if (ci > 0)
                    appendStringInfoString(&select_cmd, ", ");
                appendStringInfo(&select_cmd, "%s",
                                 quote_identifier(opts->columns[ci]));
            }
        }
        else
        {
            appendStringInfoChar(&select_cmd, '*');
        }

        appendStringInfo(&select_cmd, " FROM %s.%s",
                         quote_identifier(schema_name),
                         quote_identifier(source_table));

        if (opts->where_clause[0] != '\0')
            appendStringInfo(&select_cmd, " WHERE %s", opts->where_clause);

        appendStringInfo(&cmd, "COPY (%s) TO STDOUT WITH (FORMAT text)",
                         select_cmd.data);
        pfree(select_cmd.data);
    }
    else
    {
        appendStringInfo(&cmd,
            "COPY %s.%s TO STDOUT WITH (FORMAT text)",
            quote_identifier(schema_name),
            quote_identifier(source_table));
    }

    res = PQexec(source_conn, cmd.data);

    if (PQresultStatus(res) != PGRES_COPY_OUT)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(source_conn));
        PQclear(res);
        pfree(cmd.data);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgx_clone: COPY OUT failed on source: %s", copy_errmsg)));
    }
    PQclear(res);

    /* Start COPY IN on local — with column list if selective */
    resetStringInfo(&cmd);

    if (opts != NULL && opts->num_columns > 0)
    {
        int ci;
        appendStringInfo(&cmd, "COPY %s.%s (",
                         quote_identifier(schema_name),
                         quote_identifier(target_table));
        for (ci = 0; ci < opts->num_columns; ci++)
        {
            if (ci > 0)
                appendStringInfoString(&cmd, ", ");
            appendStringInfo(&cmd, "%s", quote_identifier(opts->columns[ci]));
        }
        appendStringInfoString(&cmd, ") FROM STDIN WITH (FORMAT text)");
    }
    else
    {
        appendStringInfo(&cmd,
            "COPY %s.%s FROM STDIN WITH (FORMAT text)",
            quote_identifier(schema_name),
            quote_identifier(target_table));
    }

    res = PQexec(local_conn, cmd.data);
    pfree(cmd.data);

    if (PQresultStatus(res) != PGRES_COPY_IN)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
        PQclear(res);

        PQgetCopyData(source_conn, &buf, 0);
        if (buf) PQfreemem(buf);

        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgx_clone: COPY IN failed on local: %s", copy_errmsg)));
    }
    PQclear(res);

    /* Stream data from source -> local */
    while ((ret = PQgetCopyData(source_conn, &buf, 0)) > 0)
    {
        if (PQputCopyData(local_conn, buf, ret) != 1)
        {
            char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
            PQfreemem(buf);
            PQputCopyEnd(local_conn, "aborted");
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("pgx_clone: error writing COPY data to local: %s",
                            copy_errmsg)));
        }

        bytes_transferred += ret;
        PQfreemem(buf);
        row_count++;

        if (row_count % 50000 == 0)
        {
            CHECK_FOR_INTERRUPTS();
            ereport(NOTICE,
                    (errmsg("pgx_clone: ... %ld rows transferred so far",
                            (long) row_count)));
        }
    }

    if (ret == -2)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(source_conn));
        PQputCopyEnd(local_conn, "source error");
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgx_clone: COPY stream error from source: %s",
                        copy_errmsg)));
    }

    if (PQputCopyEnd(local_conn, NULL) != 1)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgx_clone: error finalizing COPY on local: %s",
                        copy_errmsg)));
    }

    res = PQgetResult(local_conn);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
        PQclear(res);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgx_clone: COPY completed with error: %s",
                        copy_errmsg)));
    }

    row_count = atol(PQcmdTuples(res));
    PQclear(res);

    return row_count;
}

/* ---------------------------------------------------------------
 * Internal helper: clone indexes for a table from source.
 * --------------------------------------------------------------- */
static int
pgx_clone_indexes(PGconn *source_conn, PGconn *target_conn,
                  const char *schema_name, const char *source_table,
                  const char *target_table)
{
    PGresult       *res;
    StringInfoData  buf;
    int             i, count, created = 0;

    initStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT pg_get_indexdef(i.indexrelid) AS indexdef "
        "FROM pg_catalog.pg_index i "
        "JOIN pg_catalog.pg_class c ON c.oid = i.indrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relname = %s "
        "AND NOT i.indisprimary "
        "AND NOT EXISTS ("
        "    SELECT 1 FROM pg_catalog.pg_constraint con "
        "    WHERE con.conindid = i.indexrelid"
        ")",
        quote_literal_cstr(schema_name),
        quote_literal_cstr(source_table));

    res = pgx_clone_exec(source_conn, buf.data);

    count = PQntuples(res);
    for (i = 0; i < count; i++)
    {
        char *indexdef = PQgetvalue(res, i, 0);

        if (strcmp(source_table, target_table) != 0)
        {
            StringInfoData  new_def;
            char           *pos;
            char            search_str[NAMEDATALEN * 2 + 8];

            snprintf(search_str, sizeof(search_str), " ON %s.%s ",
                     quote_identifier(schema_name),
                     quote_identifier(source_table));

            pos = strstr(indexdef, search_str);
            if (pos != NULL)
            {
                initStringInfo(&new_def);
                appendBinaryStringInfo(&new_def, indexdef, pos - indexdef);
                appendStringInfo(&new_def, " ON %s.%s ",
                                 quote_identifier(schema_name),
                                 quote_identifier(target_table));
                appendStringInfoString(&new_def, pos + strlen(search_str));

                if (pgx_clone_exec_conn(target_conn, new_def.data))
                    created++;
                pfree(new_def.data);
            }
            else
            {
                if (pgx_clone_exec_conn(target_conn, indexdef))
                    created++;
            }
        }
        else
        {
            if (pgx_clone_exec_conn(target_conn, indexdef))
                created++;
        }
    }

    PQclear(res);
    pfree(buf.data);

    return created;
}

/* ---------------------------------------------------------------
 * Internal helper: clone constraints for a table from source.
 * --------------------------------------------------------------- */
static int
pgx_clone_constraints(PGconn *source_conn, PGconn *target_conn,
                      const char *schema_name, const char *source_table,
                      const char *target_table)
{
    PGresult       *res;
    StringInfoData  buf;
    int             i, count, created = 0;

    initStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT conname, contype, "
        "pg_get_constraintdef(con.oid, true) AS condef "
        "FROM pg_catalog.pg_constraint con "
        "JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relname = %s "
        "ORDER BY "
        "  CASE contype "
        "    WHEN 'p' THEN 1 "
        "    WHEN 'u' THEN 2 "
        "    WHEN 'c' THEN 3 "
        "    WHEN 'f' THEN 4 "
        "    ELSE 5 "
        "  END, conname",
        quote_literal_cstr(schema_name),
        quote_literal_cstr(source_table));

    res = pgx_clone_exec(source_conn, buf.data);

    count = PQntuples(res);
    for (i = 0; i < count; i++)
    {
        const char *conname = PQgetvalue(res, i, 0);
        const char *condef  = PQgetvalue(res, i, 2);

        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "ALTER TABLE %s.%s ADD CONSTRAINT %s %s",
            quote_identifier(schema_name),
            quote_identifier(target_table),
            quote_identifier(conname),
            condef);

        if (pgx_clone_exec_conn(target_conn, buf.data))
            created++;
    }

    PQclear(res);
    pfree(buf.data);

    return created;
}

/* ---------------------------------------------------------------
 * Internal helper: clone triggers for a table from source.
 * --------------------------------------------------------------- */
static int
pgx_clone_triggers(PGconn *source_conn, PGconn *target_conn,
                   const char *schema_name, const char *source_table,
                   const char *target_table)
{
    PGresult       *res;
    StringInfoData  buf;
    int             i, count, created = 0;

    initStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT pg_get_triggerdef(t.oid, true) AS trigdef "
        "FROM pg_catalog.pg_trigger t "
        "JOIN pg_catalog.pg_class c ON c.oid = t.tgrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relname = %s "
        "AND NOT t.tgisinternal "
        "ORDER BY t.tgname",
        quote_literal_cstr(schema_name),
        quote_literal_cstr(source_table));

    res = pgx_clone_exec(source_conn, buf.data);

    count = PQntuples(res);
    for (i = 0; i < count; i++)
    {
        char *trigdef = PQgetvalue(res, i, 0);

        if (strcmp(source_table, target_table) != 0)
        {
            StringInfoData  new_def;
            char           *pos;
            char            search_str[NAMEDATALEN * 2 + 8];

            snprintf(search_str, sizeof(search_str), " ON %s.%s ",
                     quote_identifier(schema_name),
                     quote_identifier(source_table));

            pos = strstr(trigdef, search_str);
            if (pos != NULL)
            {
                initStringInfo(&new_def);
                appendBinaryStringInfo(&new_def, trigdef, pos - trigdef);
                appendStringInfo(&new_def, " ON %s.%s ",
                                 quote_identifier(schema_name),
                                 quote_identifier(target_table));
                appendStringInfoString(&new_def, pos + strlen(search_str));

                if (pgx_clone_exec_conn(target_conn, new_def.data))
                    created++;
                pfree(new_def.data);
            }
            else
            {
                if (pgx_clone_exec_conn(target_conn, trigdef))
                    created++;
            }
        }
        else
        {
            if (pgx_clone_exec_conn(target_conn, trigdef))
                created++;
        }
    }

    PQclear(res);
    pfree(buf.data);

    return created;
}

/* ===============================================================
 * FUNCTION: pgx_clone_table
 *
 * Overloads:
 *   pgx_clone_table(conninfo, schema, table, include_data)
 *   pgx_clone_table(conninfo, schema, table, include_data, target_name)
 *   pgx_clone_table(conninfo, schema, table, include_data, target_name, options_json)
 *   pgx_clone_table_ex(conninfo, schema, table, include_data, target_name,
 *                      include_indexes, include_constraints, include_triggers)
 *
 * All variants go through the same C function. Arguments are
 * detected by PG_NARGS() and PG_ARGISNULL().
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_table);

Datum
pgx_clone_table(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    text       *tablename_t       = PG_GETARG_TEXT_PP(2);
    bool        include_data      = PG_GETARG_BOOL(3);

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);
    char       *table_name        = text_to_cstring(tablename_t);
    char       *target_name;
    CloneOptions opts             = pgx_clone_default_options();

    PGconn     *source_conn;
    PGconn     *local_conn;
    PGresult   *res;
    StringInfoData buf;

    /* Arg 4: target table name (optional) */
    if (PG_NARGS() >= 5 && !PG_ARGISNULL(4))
        target_name = text_to_cstring(PG_GETARG_TEXT_PP(4));
    else
        target_name = table_name;

    /* Arg 5: options — could be JSON text (6 args) */
    if (PG_NARGS() == 6 && !PG_ARGISNULL(5))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(5));
        opts = pgx_clone_parse_options(options_json);
        pfree(options_json);
    }

    /* Args 5,6,7: boolean overload (8 args via pgx_clone_table_ex) */
    if (PG_NARGS() == 8)
    {
        if (!PG_ARGISNULL(5))
            opts.include_indexes = PG_GETARG_BOOL(5);
        if (!PG_ARGISNULL(6))
            opts.include_constraints = PG_GETARG_BOOL(6);
        if (!PG_ARGISNULL(7))
            opts.include_triggers = PG_GETARG_BOOL(7);
    }

    /* Connect to source */
    source_conn = pgx_clone_connect(source_conninfo);

    /* ---- Step 1: Get CREATE TABLE DDL from source ---- */
    initStringInfo(&buf);

    if (opts.num_columns > 0)
    {
        /* Build column filter: AND a.attname IN ('col1', 'col2', ...) */
        StringInfoData col_filter;
        int ci;

        initStringInfo(&col_filter);
        appendStringInfoString(&col_filter, "AND a.attname IN (");
        for (ci = 0; ci < opts.num_columns; ci++)
        {
            if (ci > 0)
                appendStringInfoString(&col_filter, ", ");
            appendStringInfo(&col_filter, "%s",
                             quote_literal_cstr(opts.columns[ci]));
        }
        appendStringInfoChar(&col_filter, ')');

        appendStringInfo(&buf,
            "SELECT 'CREATE TABLE IF NOT EXISTS %s.%s (' || "
            "string_agg(quote_ident(a.attname) || ' ' || "
            "pg_catalog.format_type(a.atttypid, a.atttypmod) || "
            "CASE WHEN a.attnotnull THEN ' NOT NULL' ELSE '' END || "
            "CASE WHEN d.adbin IS NOT NULL THEN ' DEFAULT ' || pg_get_expr(d.adbin, d.adrelid) ELSE '' END, "
            "', ' ORDER BY a.attnum) || ')' AS ddl "
            "FROM pg_catalog.pg_class c "
            "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
            "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid "
            "LEFT JOIN pg_catalog.pg_attrdef d ON d.adrelid = c.oid AND d.adnum = a.attnum "
            "WHERE n.nspname = %s AND c.relname = %s "
            "AND a.attnum > 0 AND NOT a.attisdropped "
            "%s "
            "GROUP BY c.relname",
            quote_identifier(schema_name),
            quote_identifier(target_name),
            quote_literal_cstr(schema_name),
            quote_literal_cstr(table_name),
            col_filter.data);

        pfree(col_filter.data);
    }
    else
    {
        appendStringInfo(&buf,
            "SELECT 'CREATE TABLE IF NOT EXISTS %s.%s (' || "
            "string_agg(quote_ident(a.attname) || ' ' || "
            "pg_catalog.format_type(a.atttypid, a.atttypmod) || "
            "CASE WHEN a.attnotnull THEN ' NOT NULL' ELSE '' END || "
            "CASE WHEN d.adbin IS NOT NULL THEN ' DEFAULT ' || pg_get_expr(d.adbin, d.adrelid) ELSE '' END, "
            "', ' ORDER BY a.attnum) || ')' AS ddl "
            "FROM pg_catalog.pg_class c "
            "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
            "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid "
            "LEFT JOIN pg_catalog.pg_attrdef d ON d.adrelid = c.oid AND d.adnum = a.attnum "
            "WHERE n.nspname = %s AND c.relname = %s "
            "AND a.attnum > 0 AND NOT a.attisdropped "
            "GROUP BY c.relname",
            quote_identifier(schema_name),
            quote_identifier(target_name),
            quote_literal_cstr(schema_name),
            quote_literal_cstr(table_name));
    }

    res = pgx_clone_exec(source_conn, buf.data);

    if (PQntuples(res) == 0)
    {
        PQclear(res);
        PQfinish(source_conn);
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("pgx_clone: table \"%s.%s\" not found on source",
                        schema_name, table_name)));
    }

    /* Use loopback libpq connection for ALL local operations */
    local_conn = pgx_clone_connect_local();

    /* ---- Step 2: Create schema + table via loopback ---- */
    {
        PGresult *local_res;
        char     *ddl;

        ddl = pstrdup(PQgetvalue(res, 0, 0));
        PQclear(res);

        resetStringInfo(&buf);
        appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                         quote_identifier(schema_name));
        pgx_clone_exec_conn(local_conn, buf.data);

        local_res = PQexec(local_conn, ddl);
        if (PQresultStatus(local_res) != PGRES_COMMAND_OK)
        {
            char *ddl_errmsg = pstrdup(PQerrorMessage(local_conn));
            PQclear(local_res);
            PQfinish(local_conn);
            PQfinish(source_conn);
            pfree(ddl);
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("pgx_clone: failed to create table locally: %s",
                            ddl_errmsg)));
        }
        PQclear(local_res);
        pfree(ddl);
    }

    /* ---- Step 3: Copy data if requested ---- */
    if (include_data)
    {
        int64 row_count;

        row_count = pgx_clone_copy_data(source_conn, local_conn,
                                        schema_name, table_name, target_name,
                                        &opts);

        ereport(NOTICE,
                (errmsg("pgx_clone: copied %ld rows into %s.%s using COPY protocol",
                        (long) row_count, schema_name, target_name)));
    }

    /* ---- Step 4: Clone constraints if enabled ---- */
    if (opts.include_constraints)
    {
        int con_count = pgx_clone_constraints(source_conn, local_conn,
                                              schema_name, table_name, target_name);
        if (con_count > 0)
            ereport(NOTICE,
                    (errmsg("pgx_clone: cloned %d constraints for %s.%s",
                            con_count, schema_name, target_name)));
    }

    /* ---- Step 5: Clone indexes if enabled ---- */
    if (opts.include_indexes)
    {
        int idx_count = pgx_clone_indexes(source_conn, local_conn,
                                          schema_name, table_name, target_name);
        if (idx_count > 0)
            ereport(NOTICE,
                    (errmsg("pgx_clone: cloned %d indexes for %s.%s",
                            idx_count, schema_name, target_name)));
    }

    /* ---- Step 6: Clone triggers if enabled ---- */
    if (opts.include_triggers)
    {
        int trig_count = pgx_clone_triggers(source_conn, local_conn,
                                            schema_name, table_name, target_name);
        if (trig_count > 0)
            ereport(NOTICE,
                    (errmsg("pgx_clone: cloned %d triggers for %s.%s",
                            trig_count, schema_name, target_name)));
    }

    PQfinish(local_conn);
    PQfinish(source_conn);

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgx_clone_table_ex
 *
 * Boolean overload: separate params for indexes, constraints, triggers
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_table_ex);

Datum
pgx_clone_table_ex(PG_FUNCTION_ARGS)
{
    /* Just forward to pgx_clone_table — same C function handles both */
    return pgx_clone_table(fcinfo);
}

/* ===============================================================
 * FUNCTION: pgx_clone_schema(source_conninfo, schema, include_data [, options])
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_schema);

Datum
pgx_clone_schema(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    bool        include_data      = PG_GETARG_BOOL(2);
    CloneOptions opts             = pgx_clone_default_options();

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);

    PGconn     *source_conn;
    PGconn     *local_conn;
    PGresult   *res;
    StringInfoData buf;
    int         i, ntables;
    char      **table_names = NULL;

    /* Arg 3: JSON options (optional) */
    if (PG_NARGS() >= 4 && !PG_ARGISNULL(3))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(3));
        opts = pgx_clone_parse_options(options_json);
        pfree(options_json);
    }

    /* Arg 3,4,5: boolean overload (6 args via pgx_clone_schema_ex) */
    if (PG_NARGS() == 6)
    {
        if (!PG_ARGISNULL(3))
            opts.include_indexes = PG_GETARG_BOOL(3);
        if (!PG_ARGISNULL(4))
            opts.include_constraints = PG_GETARG_BOOL(4);
        if (!PG_ARGISNULL(5))
            opts.include_triggers = PG_GETARG_BOOL(5);
    }

    source_conn = pgx_clone_connect(source_conninfo);
    local_conn = pgx_clone_connect_local();

    /* ---- Step 1: Create schema ---- */
    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                     quote_identifier(schema_name));
    pgx_clone_exec_conn(local_conn, buf.data);

    /* ---- Step 2: Clone all functions/procedures ---- */
    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT pg_get_functiondef(p.oid) AS funcdef "
        "FROM pg_catalog.pg_proc p "
        "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
        "WHERE n.nspname = %s",
        quote_literal_cstr(schema_name));

    res = pgx_clone_exec(source_conn, buf.data);

    for (i = 0; i < PQntuples(res); i++)
    {
        pgx_clone_exec_conn(local_conn, PQgetvalue(res, i, 0));
    }
    ereport(NOTICE,
            (errmsg("pgx_clone: cloned %d functions from schema %s",
                    PQntuples(res), schema_name)));
    PQclear(res);

    /* ---- Step 3: Clone sequences ---- */
    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT sequence_name, data_type, start_value, increment, "
        "minimum_value, maximum_value, cycle_option "
        "FROM information_schema.sequences "
        "WHERE sequence_schema = %s",
        quote_literal_cstr(schema_name));

    res = pgx_clone_exec(source_conn, buf.data);

    for (i = 0; i < PQntuples(res); i++)
    {
        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "CREATE SEQUENCE IF NOT EXISTS %s.%s "
            "AS %s START WITH %s INCREMENT BY %s "
            "MINVALUE %s MAXVALUE %s %s CYCLE",
            quote_identifier(schema_name),
            quote_identifier(PQgetvalue(res, i, 0)),
            PQgetvalue(res, i, 1),
            PQgetvalue(res, i, 2),
            PQgetvalue(res, i, 3),
            PQgetvalue(res, i, 4),
            PQgetvalue(res, i, 5),
            strcmp(PQgetvalue(res, i, 6), "YES") == 0 ? "" : "NO");
        pgx_clone_exec_conn(local_conn, buf.data);
    }
    ereport(NOTICE,
            (errmsg("pgx_clone: cloned %d sequences from schema %s",
                    PQntuples(res), schema_name)));
    PQclear(res);

    PQfinish(local_conn);

    /* ---- Step 4: Clone tables ---- */
    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT tablename FROM pg_catalog.pg_tables "
        "WHERE schemaname = %s ORDER BY tablename",
        quote_literal_cstr(schema_name));

    res = pgx_clone_exec(source_conn, buf.data);

    ntables = PQntuples(res);

    if (ntables > 0)
    {
        table_names = palloc(sizeof(char *) * ntables);
        for (i = 0; i < ntables; i++)
            table_names[i] = pstrdup(PQgetvalue(res, i, 0));
    }
    PQclear(res);
    PQfinish(source_conn);

    /* Build options JSON to pass through to pgx_clone_table */
    {
        StringInfoData opts_json;
        initStringInfo(&opts_json);
        appendStringInfo(&opts_json,
            "{\"indexes\": %s, \"constraints\": %s, \"triggers\": %s}",
            opts.include_indexes ? "true" : "false",
            opts.include_constraints ? "true" : "false",
            opts.include_triggers ? "true" : "false");

        for (i = 0; i < ntables; i++)
        {
            Datum result;

            /* Call 6-arg version: conninfo, schema, table, include_data, target_name, options */
            result = DirectFunctionCall6(pgx_clone_table,
                        CStringGetTextDatum(source_conninfo),
                        CStringGetTextDatum(schema_name),
                        CStringGetTextDatum(table_names[i]),
                        BoolGetDatum(include_data),
                        CStringGetTextDatum(table_names[i]),  /* target = source name */
                        CStringGetTextDatum(opts_json.data));
            (void) result;
        }

        pfree(opts_json.data);
    }

    ereport(NOTICE,
            (errmsg("pgx_clone: cloned %d tables from schema %s",
                    ntables, schema_name)));

    /* ---- Step 5: Retry FK constraints if constraints enabled ---- */
    if (opts.include_constraints)
    {
        PGconn *src_retry  = pgx_clone_connect(source_conninfo);
        PGconn *lcl_retry  = pgx_clone_connect_local();
        int     fk_created = 0;

        for (i = 0; i < ntables; i++)
        {
            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "SELECT conname, pg_get_constraintdef(con.oid, true) AS condef "
                "FROM pg_catalog.pg_constraint con "
                "JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "WHERE n.nspname = %s AND c.relname = %s "
                "AND contype = 'f'",
                quote_literal_cstr(schema_name),
                quote_literal_cstr(table_names[i]));

            res = pgx_clone_exec(src_retry, buf.data);

            {
                int j;
                for (j = 0; j < PQntuples(res); j++)
                {
                    const char *conname = PQgetvalue(res, j, 0);
                    const char *condef  = PQgetvalue(res, j, 1);

                    resetStringInfo(&buf);
                    appendStringInfo(&buf,
                        "DO $$ BEGIN "
                        "ALTER TABLE %s.%s ADD CONSTRAINT %s %s; "
                        "EXCEPTION WHEN duplicate_object THEN NULL; "
                        "END $$",
                        quote_identifier(schema_name),
                        quote_identifier(table_names[i]),
                        quote_identifier(conname),
                        condef);

                    if (pgx_clone_exec_conn(lcl_retry, buf.data))
                        fk_created++;
                }
            }

            PQclear(res);
        }

        if (fk_created > 0)
            ereport(NOTICE,
                    (errmsg("pgx_clone: FK retry pass: ensured %d foreign key constraints in schema %s",
                            fk_created, schema_name)));

        PQfinish(lcl_retry);
        PQfinish(src_retry);
    }

    /* ---- Step 6: Clone views ---- */
    {
        PGconn *src_views = pgx_clone_connect(source_conninfo);
        PGconn *lcl_views = pgx_clone_connect_local();

        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "SELECT table_name, view_definition "
            "FROM information_schema.views "
            "WHERE table_schema = %s",
            quote_literal_cstr(schema_name));

        res = pgx_clone_exec(src_views, buf.data);

        for (i = 0; i < PQntuples(res); i++)
        {
            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "CREATE OR REPLACE VIEW %s.%s AS %s",
                quote_identifier(schema_name),
                quote_identifier(PQgetvalue(res, i, 0)),
                PQgetvalue(res, i, 1));
            pgx_clone_exec_conn(lcl_views, buf.data);
        }
        ereport(NOTICE,
                (errmsg("pgx_clone: cloned %d views from schema %s",
                        PQntuples(res), schema_name)));
        PQclear(res);

        PQfinish(lcl_views);
        PQfinish(src_views);
    }

    if (table_names)
    {
        for (i = 0; i < ntables; i++)
            pfree(table_names[i]);
        pfree(table_names);
    }

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* pgx_clone_schema_ex — boolean overload for schema */
PG_FUNCTION_INFO_V1(pgx_clone_schema_ex);

Datum
pgx_clone_schema_ex(PG_FUNCTION_ARGS)
{
    return pgx_clone_schema(fcinfo);
}

/* ===============================================================
 * FUNCTION: pgx_clone_functions(source_conninfo, schema)
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_functions);

Datum
pgx_clone_functions(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);

    PGconn     *source_conn;
    PGconn     *local_conn;
    PGresult   *res;
    StringInfoData buf;
    int         i, count;

    source_conn = pgx_clone_connect(source_conninfo);
    local_conn  = pgx_clone_connect_local();

    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                     quote_identifier(schema_name));
    pgx_clone_exec_conn(local_conn, buf.data);

    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT pg_get_functiondef(p.oid) AS funcdef "
        "FROM pg_catalog.pg_proc p "
        "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
        "WHERE n.nspname = %s",
        quote_literal_cstr(schema_name));

    res = pgx_clone_exec(source_conn, buf.data);

    count = PQntuples(res);
    for (i = 0; i < count; i++)
    {
        pgx_clone_exec_conn(local_conn, PQgetvalue(res, i, 0));
    }

    PQclear(res);
    PQfinish(local_conn);
    PQfinish(source_conn);

    ereport(NOTICE,
            (errmsg("pgx_clone: successfully cloned %d functions from %s",
                    count, schema_name)));

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgx_clone_database(source_conninfo, include_data [, options])
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_database);

Datum
pgx_clone_database(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    bool        include_data      = PG_GETARG_BOOL(1);
    CloneOptions opts             = pgx_clone_default_options();

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);

    PGconn     *source_conn;
    PGresult   *res;
    int         i, nschemas;
    char      **schema_names;

    /* Arg 2: JSON options (optional) */
    if (PG_NARGS() >= 3 && !PG_ARGISNULL(2))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(2));
        opts = pgx_clone_parse_options(options_json);
        pfree(options_json);
    }

    source_conn = pgx_clone_connect(source_conninfo);

    res = pgx_clone_exec(source_conn,
        "SELECT nspname FROM pg_catalog.pg_namespace "
        "WHERE nspname NOT LIKE 'pg_%' "
        "AND nspname <> 'information_schema' "
        "ORDER BY nspname");

    nschemas = PQntuples(res);

    schema_names = palloc(sizeof(char *) * nschemas);
    for (i = 0; i < nschemas; i++)
        schema_names[i] = pstrdup(PQgetvalue(res, i, 0));

    PQclear(res);
    PQfinish(source_conn);

    {
        StringInfoData opts_json;
        initStringInfo(&opts_json);
        appendStringInfo(&opts_json,
            "{\"indexes\": %s, \"constraints\": %s, \"triggers\": %s}",
            opts.include_indexes ? "true" : "false",
            opts.include_constraints ? "true" : "false",
            opts.include_triggers ? "true" : "false");

        for (i = 0; i < nschemas; i++)
        {
            Datum result;

            ereport(NOTICE,
                    (errmsg("pgx_clone: cloning schema %s (%d/%d)",
                            schema_names[i], i + 1, nschemas)));

            result = DirectFunctionCall4(pgx_clone_schema,
                        CStringGetTextDatum(source_conninfo),
                        CStringGetTextDatum(schema_names[i]),
                        BoolGetDatum(include_data),
                        CStringGetTextDatum(opts_json.data));
            (void) result;
        }

        pfree(opts_json.data);
    }

    ereport(NOTICE,
            (errmsg("pgx_clone: database clone complete — %d schemas cloned",
                    nschemas)));

    for (i = 0; i < nschemas; i++)
        pfree(schema_names[i]);
    pfree(schema_names);

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgx_clone_version()
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_version);

Datum
pgx_clone_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("pgx_clone 1.1.0"));
}

/* ===============================================================
 * _PG_init — called when the shared library is loaded.
 * Registers shared memory for job tracking.
 * Required: shared_preload_libraries = 'pgx_clone'
 * =============================================================== */
void _PG_init(void);

void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
        return;

    pgx_clone_shmem_init();
}

/* ===============================================================
 * ASYNC CLONE FUNCTIONS
 *
 * Submit clone jobs to background workers for non-blocking
 * operations with progress tracking, cancel, and resume.
 * =============================================================== */

/* ===============================================================
 * FUNCTION: pgx_clone_table_async(conninfo, schema, table,
 *              include_data [, target_name [, options_json]])
 *
 * Returns job_id (INTEGER).
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_table_async);

Datum
pgx_clone_table_async(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    text       *tablename_t       = PG_GETARG_TEXT_PP(2);
    bool        include_data      = PG_GETARG_BOOL(3);

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);
    char       *table_name        = text_to_cstring(tablename_t);
    char       *target_name       = table_name;
    CloneOptions opts             = pgx_clone_default_options();
    PgxCloneConflictStrategy conflict = PGX_CLONE_CONFLICT_ERROR;

    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;
    PgxCloneJob    *job;
    int             job_id;

    if (PG_NARGS() >= 5 && !PG_ARGISNULL(4))
        target_name = text_to_cstring(PG_GETARG_TEXT_PP(4));

    if (PG_NARGS() >= 6 && !PG_ARGISNULL(5))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(5));
        opts = pgx_clone_parse_options(options_json);

        if (strstr(options_json, "\"conflict\": \"skip\"") ||
            strstr(options_json, "\"conflict\":\"skip\""))
            conflict = PGX_CLONE_CONFLICT_SKIP;
        else if (strstr(options_json, "\"conflict\": \"replace\"") ||
                 strstr(options_json, "\"conflict\":\"replace\""))
            conflict = PGX_CLONE_CONFLICT_REPLACE;
        else if (strstr(options_json, "\"conflict\": \"rename\"") ||
                 strstr(options_json, "\"conflict\":\"rename\""))
            conflict = PGX_CLONE_CONFLICT_RENAME;

        pfree(options_json);
    }

    if (!pgx_clone_state)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pgx_clone: shared memory not initialized"),
                 errhint("Add pgx_clone to shared_preload_libraries in postgresql.conf")));

    LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);

    job = find_free_slot();
    if (!job)
    {
        LWLockRelease(pgx_clone_state->lock);
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                 errmsg("pgx_clone: no free job slots (max %d)", PGX_CLONE_MAX_JOBS)));
    }

    job_id = pgx_clone_state->next_job_id++;
    memset(job, 0, sizeof(PgxCloneJob));
    job->job_id = job_id;
    job->status = PGX_CLONE_JOB_PENDING;
    job->op_type = PGX_CLONE_OP_TABLE;
    job->database_oid = MyDatabaseId;

    strlcpy(job->source_conninfo, source_conninfo, sizeof(job->source_conninfo));
    strlcpy(job->schema_name, schema_name, NAMEDATALEN);
    strlcpy(job->table_name, table_name, NAMEDATALEN);
    strlcpy(job->target_name, target_name, NAMEDATALEN);

    job->include_data        = include_data;
    job->include_indexes     = opts.include_indexes;
    job->include_constraints = opts.include_constraints;
    job->include_triggers    = opts.include_triggers;
    job->conflict_strategy   = conflict;
    job->resumable           = true;

    LWLockRelease(pgx_clone_state->lock);

    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "pgx_clone: job %d (%s.%s)",
             job_id, schema_name, table_name);
    snprintf(worker.bgw_type, BGW_MAXLEN, "pgx_clone worker");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgx_clone");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgx_clone_bgw_main");
    worker.bgw_main_arg = Int32GetDatum(job_id);
    worker.bgw_notify_pid = MyProcPid;

    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
    {
        LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
        job->status = PGX_CLONE_JOB_FREE;
        LWLockRelease(pgx_clone_state->lock);
        ereport(ERROR, (errmsg("pgx_clone: could not register background worker")));
    }

    PG_RETURN_INT32(job_id);
}

/* ===============================================================
 * FUNCTION: pgx_clone_schema_async(conninfo, schema, include_data
 *              [, options_json])
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_schema_async);

Datum
pgx_clone_schema_async(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    bool        include_data      = PG_GETARG_BOOL(2);
    CloneOptions opts             = pgx_clone_default_options();
    PgxCloneConflictStrategy conflict = PGX_CLONE_CONFLICT_ERROR;

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);

    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;
    PgxCloneJob    *job;
    int             job_id;

    if (PG_NARGS() >= 4 && !PG_ARGISNULL(3))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(3));
        opts = pgx_clone_parse_options(options_json);

        if (strstr(options_json, "\"conflict\": \"skip\"") ||
            strstr(options_json, "\"conflict\":\"skip\""))
            conflict = PGX_CLONE_CONFLICT_SKIP;
        else if (strstr(options_json, "\"conflict\": \"replace\"") ||
                 strstr(options_json, "\"conflict\":\"replace\""))
            conflict = PGX_CLONE_CONFLICT_REPLACE;
        else if (strstr(options_json, "\"conflict\": \"rename\"") ||
                 strstr(options_json, "\"conflict\":\"rename\""))
            conflict = PGX_CLONE_CONFLICT_RENAME;

        pfree(options_json);
    }

    if (!pgx_clone_state)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pgx_clone: shared memory not initialized"),
                 errhint("Add pgx_clone to shared_preload_libraries")));

    LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);

    job = find_free_slot();
    if (!job)
    {
        LWLockRelease(pgx_clone_state->lock);
        ereport(ERROR, (errmsg("pgx_clone: no free job slots")));
    }

    job_id = pgx_clone_state->next_job_id++;
    memset(job, 0, sizeof(PgxCloneJob));
    job->job_id = job_id;
    job->status = PGX_CLONE_JOB_PENDING;
    job->op_type = PGX_CLONE_OP_SCHEMA;
    job->database_oid = MyDatabaseId;

    strlcpy(job->source_conninfo, source_conninfo, sizeof(job->source_conninfo));
    strlcpy(job->schema_name, schema_name, NAMEDATALEN);

    job->include_data        = include_data;
    job->include_indexes     = opts.include_indexes;
    job->include_constraints = opts.include_constraints;
    job->include_triggers    = opts.include_triggers;
    job->conflict_strategy   = conflict;
    job->resumable           = true;

    LWLockRelease(pgx_clone_state->lock);

    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "pgx_clone: schema %s (job %d)",
             schema_name, job_id);
    snprintf(worker.bgw_type, BGW_MAXLEN, "pgx_clone worker");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgx_clone");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgx_clone_bgw_main");
    worker.bgw_main_arg = Int32GetDatum(job_id);
    worker.bgw_notify_pid = MyProcPid;

    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
    {
        LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
        job->status = PGX_CLONE_JOB_FREE;
        LWLockRelease(pgx_clone_state->lock);
        ereport(ERROR, (errmsg("pgx_clone: could not register background worker")));
    }

    PG_RETURN_INT32(job_id);
}

/* ===============================================================
 * FUNCTION: pgx_clone_progress(job_id) — returns JSON
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_progress);

Datum
pgx_clone_progress(PG_FUNCTION_ARGS)
{
    int             job_id = PG_GETARG_INT32(0);
    PgxCloneJob    *job;
    StringInfoData  result;
    const char     *status_str;

    if (!pgx_clone_state)
        ereport(ERROR, (errmsg("pgx_clone: shared memory not initialized")));

    LWLockAcquire(pgx_clone_state->lock, LW_SHARED);
    job = find_job(job_id);

    if (!job)
    {
        LWLockRelease(pgx_clone_state->lock);
        ereport(ERROR, (errmsg("pgx_clone: job %d not found", job_id)));
    }

    switch (job->status)
    {
        case PGX_CLONE_JOB_PENDING:   status_str = "pending";   break;
        case PGX_CLONE_JOB_RUNNING:   status_str = "running";   break;
        case PGX_CLONE_JOB_COMPLETED: status_str = "completed"; break;
        case PGX_CLONE_JOB_FAILED:    status_str = "failed";    break;
        case PGX_CLONE_JOB_CANCELLED: status_str = "cancelled"; break;
        default:                      status_str = "unknown";    break;
    }

    initStringInfo(&result);
    appendStringInfo(&result,
        "{\"job_id\": %d, \"status\": \"%s\", \"phase\": \"%s\", "
        "\"tables_completed\": %ld, \"tables_total\": %ld, "
        "\"rows_copied\": %ld, \"current_table\": \"%s\"",
        job->job_id, status_str, job->current_phase,
        (long) job->completed_tables, (long) job->total_tables,
        (long) job->copied_rows, job->current_table);

    if (job->status == PGX_CLONE_JOB_FAILED)
        appendStringInfo(&result, ", \"error\": \"%s\"", job->error_message);

    if (job->resume_checkpoint[0] != '\0')
        appendStringInfo(&result, ", \"checkpoint\": \"%s\"", job->resume_checkpoint);

    if (job->start_time != 0)
    {
        long elapsed_ms;
        if (job->end_time != 0)
            elapsed_ms = (long)((job->end_time - job->start_time) / 1000);
        else
            elapsed_ms = (long)((GetCurrentTimestamp() - job->start_time) / 1000);
        appendStringInfo(&result, ", \"elapsed_ms\": %ld", elapsed_ms);
    }

    appendStringInfoChar(&result, '}');
    LWLockRelease(pgx_clone_state->lock);

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/* ===============================================================
 * FUNCTION: pgx_clone_cancel(job_id)
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_cancel);

Datum
pgx_clone_cancel(PG_FUNCTION_ARGS)
{
    int             job_id = PG_GETARG_INT32(0);
    PgxCloneJob    *job;

    if (!pgx_clone_state)
        ereport(ERROR, (errmsg("pgx_clone: shared memory not initialized")));

    LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
    job = find_job(job_id);

    if (!job)
    {
        LWLockRelease(pgx_clone_state->lock);
        ereport(ERROR, (errmsg("pgx_clone: job %d not found", job_id)));
    }

    if (job->status == PGX_CLONE_JOB_RUNNING ||
        job->status == PGX_CLONE_JOB_PENDING)
    {
        job->status = PGX_CLONE_JOB_CANCELLED;
        job->end_time = GetCurrentTimestamp();
        strlcpy(job->current_phase, "cancelled", 64);
    }

    LWLockRelease(pgx_clone_state->lock);

    PG_RETURN_TEXT_P(cstring_to_text("cancelled"));
}

/* ===============================================================
 * FUNCTION: pgx_clone_resume(job_id) — returns new job_id
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_resume);

Datum
pgx_clone_resume(PG_FUNCTION_ARGS)
{
    int             old_job_id = PG_GETARG_INT32(0);
    PgxCloneJob    *old_job, *new_job;
    int             new_job_id;
    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;

    if (!pgx_clone_state)
        ereport(ERROR, (errmsg("pgx_clone: shared memory not initialized")));

    LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);

    old_job = find_job(old_job_id);
    if (!old_job)
    {
        LWLockRelease(pgx_clone_state->lock);
        ereport(ERROR, (errmsg("pgx_clone: job %d not found", old_job_id)));
    }

    if (old_job->status != PGX_CLONE_JOB_FAILED &&
        old_job->status != PGX_CLONE_JOB_CANCELLED)
    {
        LWLockRelease(pgx_clone_state->lock);
        ereport(ERROR, (errmsg("pgx_clone: job %d is not resumable", old_job_id)));
    }

    new_job = find_free_slot();
    if (!new_job)
    {
        LWLockRelease(pgx_clone_state->lock);
        ereport(ERROR, (errmsg("pgx_clone: no free job slots")));
    }

    new_job_id = pgx_clone_state->next_job_id++;
    memcpy(new_job, old_job, sizeof(PgxCloneJob));
    new_job->job_id = new_job_id;
    new_job->status = PGX_CLONE_JOB_PENDING;
    new_job->worker_pid = 0;
    new_job->start_time = 0;
    new_job->end_time = 0;
    new_job->error_message[0] = '\0';
    /* resume_checkpoint preserved — bgworker will skip past it */

    old_job->status = PGX_CLONE_JOB_FREE;

    LWLockRelease(pgx_clone_state->lock);

    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "pgx_clone: resume job %d", new_job_id);
    snprintf(worker.bgw_type, BGW_MAXLEN, "pgx_clone worker");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgx_clone");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgx_clone_bgw_main");
    worker.bgw_main_arg = Int32GetDatum(new_job_id);
    worker.bgw_notify_pid = MyProcPid;

    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
    {
        LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
        new_job->status = PGX_CLONE_JOB_FREE;
        LWLockRelease(pgx_clone_state->lock);
        ereport(ERROR, (errmsg("pgx_clone: could not register background worker")));
    }

    PG_RETURN_INT32(new_job_id);
}

/* ===============================================================
 * FUNCTION: pgx_clone_jobs() — returns JSON array of all jobs
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_jobs);

Datum
pgx_clone_jobs(PG_FUNCTION_ARGS)
{
    StringInfoData  result;
    int             i;
    bool            first = true;
    const char     *status_str;

    if (!pgx_clone_state)
        ereport(ERROR, (errmsg("pgx_clone: shared memory not initialized")));

    initStringInfo(&result);
    appendStringInfoChar(&result, '[');

    LWLockAcquire(pgx_clone_state->lock, LW_SHARED);

    for (i = 0; i < PGX_CLONE_MAX_JOBS; i++)
    {
        PgxCloneJob *j = &pgx_clone_state->jobs[i];

        if (j->status == PGX_CLONE_JOB_FREE)
            continue;

        if (!first)
            appendStringInfoChar(&result, ',');
        first = false;

        switch (j->status)
        {
            case PGX_CLONE_JOB_PENDING:   status_str = "pending";   break;
            case PGX_CLONE_JOB_RUNNING:   status_str = "running";   break;
            case PGX_CLONE_JOB_COMPLETED: status_str = "completed"; break;
            case PGX_CLONE_JOB_FAILED:    status_str = "failed";    break;
            case PGX_CLONE_JOB_CANCELLED: status_str = "cancelled"; break;
            default:                      status_str = "unknown";    break;
        }

        appendStringInfo(&result,
            "{\"job_id\": %d, \"status\": \"%s\", \"schema\": \"%s\", "
            "\"table\": \"%s\", \"phase\": \"%s\", "
            "\"tables_completed\": %ld, \"tables_total\": %ld}",
            j->job_id, status_str, j->schema_name,
            j->table_name, j->current_phase,
            (long) j->completed_tables, (long) j->total_tables);
    }

    LWLockRelease(pgx_clone_state->lock);
    appendStringInfoChar(&result, ']');

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}
