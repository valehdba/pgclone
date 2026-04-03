/*
 * pgclone - PostgreSQL extension for cloning databases, schemas, tables,
 *            and functions between PostgreSQL hosts.
 *
 * Copyright (c) 2026, Valeh Agayev pgclone contributors
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

#include "pgclone_bgw.h"

PG_MODULE_MAGIC;

/* ---------------------------------------------------------------
 * Clone options struct — controls what gets cloned beyond the
 * table structure and data.
 * --------------------------------------------------------------- */
#define PGCLONE_MAX_COLUMNS   64
#define PGCLONE_MAX_WHERE     2048

typedef struct CloneOptions
{
    bool include_indexes;
    bool include_constraints;
    bool include_triggers;
    bool include_matviews;       /* clone materialized views */

    /* Selective column cloning: if num_columns > 0, only these columns */
    int  num_columns;
    char columns[PGCLONE_MAX_COLUMNS][NAMEDATALEN];

    /* Data filtering: WHERE clause applied during COPY */
    char where_clause[PGCLONE_MAX_WHERE];

    /* Parallel cloning: number of workers (0 = sequential) */
    int  parallel_workers;
} CloneOptions;

/* Default: everything enabled, no column/where filter */
static CloneOptions
pgclone_default_options(void)
{
    CloneOptions opts;
    memset(&opts, 0, sizeof(CloneOptions));
    opts.include_indexes     = true;
    opts.include_constraints = true;
    opts.include_triggers    = true;
    opts.include_matviews    = true;
    opts.num_columns         = 0;
    opts.where_clause[0]     = '\0';
    opts.parallel_workers    = 0;
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
pgclone_parse_options(const char *json_str)
{
    CloneOptions opts = pgclone_default_options();
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

    if (strstr(json_str, "\"matviews\": false") != NULL ||
        strstr(json_str, "\"matviews\":false") != NULL)
        opts.include_matviews = false;

    /* Parse "parallel": N */
    {
        const char *pp = strstr(json_str, "\"parallel\"");
        if (pp != NULL)
        {
            const char *colon = strchr(pp, ':');
            if (colon != NULL)
            {
                int val = atoi(colon + 1);
                if (val > 0 && val <= PGCLONE_MAX_JOBS)
                    opts.parallel_workers = val;
            }
        }
    }

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

                while (cur < end_bracket && opts.num_columns < PGCLONE_MAX_COLUMNS)
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
                    if (len > 0 && len < PGCLONE_MAX_WHERE)
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
pgclone_connect(const char *conninfo)
{
    PGconn *conn;

    conn = PQconnectdb(conninfo);

    if (PQstatus(conn) != CONNECTION_OK)
    {
        char *conn_errmsg = pstrdup(PQerrorMessage(conn));
        PQfinish(conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgclone: could not connect to remote host: %s", conn_errmsg)));
    }

    return conn;
}

/* ---------------------------------------------------------------
 * Internal helper: execute a query on a remote connection
 * Returns the PGresult (caller must PQclear it)
 * --------------------------------------------------------------- */
static PGresult *
pgclone_exec(PGconn *conn, const char *query)
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
                 errmsg("pgclone: remote query failed: %s", exec_errmsg)));
    }

    return res;
}

/* ---------------------------------------------------------------
 * Internal helper: execute DDL on the local database via SPI
 * --------------------------------------------------------------- */
static void
pgclone_exec_local(const char *query)
{
    int ret;

    ret = SPI_execute(query, false, 0);

    if (ret != SPI_OK_UTILITY && ret != SPI_OK_SELECT &&
        ret != SPI_OK_INSERT && ret != SPI_OK_UPDATE)
    {
        ereport(WARNING,
                (errmsg("pgclone: local execution returned code %d for: %.128s",
                        ret, query)));
    }
}

/* ---------------------------------------------------------------
 * Internal helper: execute DDL on a libpq connection.
 * Used for loopback connection operations.
 * Returns true on success, false on failure (with WARNING).
 * --------------------------------------------------------------- */
static bool
pgclone_exec_conn(PGconn *conn, const char *query)
{
    PGresult *res;

    res = PQexec(conn, query);

    if (PQresultStatus(res) != PGRES_COMMAND_OK &&
        PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        char *conn_errmsg = pstrdup(PQerrorMessage(conn));
        PQclear(res);
        ereport(WARNING,
                (errmsg("pgclone: local exec failed: %s (query: %.128s)",
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
pgclone_connect_local(void)
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
                 errmsg("pgclone: could not connect to local database: %s",
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
pgclone_copy_data(PGconn *source_conn, PGconn *local_conn,
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
                 errmsg("pgclone: COPY OUT failed on source: %s", copy_errmsg)));
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
                 errmsg("pgclone: COPY IN failed on local: %s", copy_errmsg)));
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
                     errmsg("pgclone: error writing COPY data to local: %s",
                            copy_errmsg)));
        }

        bytes_transferred += ret;
        PQfreemem(buf);
        row_count++;

        if (row_count % 50000 == 0)
        {
            CHECK_FOR_INTERRUPTS();
            elog(DEBUG1, "pgclone: ... %ld rows transferred so far",
                 (long) row_count);
        }
    }

    if (ret == -2)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(source_conn));
        PQputCopyEnd(local_conn, "source error");
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: COPY stream error from source: %s",
                        copy_errmsg)));
    }

    if (PQputCopyEnd(local_conn, NULL) != 1)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: error finalizing COPY on local: %s",
                        copy_errmsg)));
    }

    res = PQgetResult(local_conn);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
        PQclear(res);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: COPY completed with error: %s",
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
pgclone_indexes(PGconn *source_conn, PGconn *target_conn,
                  const char *schema_name, const char *source_table,
                  const char *target_table,
                  const CloneOptions *opts)
{
    PGresult       *res;
    StringInfoData  buf;
    int             i, count, created = 0;

    initStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT pg_get_indexdef(i.indexrelid) AS indexdef, "
        "ARRAY(SELECT a.attname FROM pg_catalog.pg_attribute a "
        "      WHERE a.attrelid = c.oid "
        "      AND a.attnum = ANY(i.indkey) "
        "      AND a.attnum > 0) AS index_cols "
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

    res = pgclone_exec(source_conn, buf.data);

    count = PQntuples(res);
    for (i = 0; i < count; i++)
    {
        char           *indexdef  = pstrdup(PQgetvalue(res, i, 0));
        char           *idx_cols  = PQgetvalue(res, i, 1);
        StringInfoData  final_def;

        /* If selective column clone, skip indexes on columns not in target */
        if (opts != NULL && opts->num_columns > 0)
        {
            /* idx_cols is a postgres array like {col1,col2} */
            char *p = idx_cols;
            bool  skip = false;

            /* Strip leading { and trailing } */
            if (*p == '{') p++;
            {
                char col_copy[NAMEDATALEN * 8];
                char *tok;
                strncpy(col_copy, p, sizeof(col_copy) - 1);
                col_copy[sizeof(col_copy) - 1] = '\0';
                /* Remove trailing } */
                {
                    size_t clen = strlen(col_copy);
                    if (clen > 0 && col_copy[clen - 1] == '}')
                        col_copy[clen - 1] = '\0';
                }
                tok = strtok(col_copy, ",");
                while (tok != NULL && !skip)
                {
                    int cj;
                    bool found = false;
                    for (cj = 0; cj < opts->num_columns; cj++)
                    {
                        if (strcmp(tok, opts->columns[cj]) == 0)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        skip = true;
                    tok = strtok(NULL, ",");
                }
            }
            if (skip)
            {
                pfree(indexdef);
                continue;
            }
        }

        initStringInfo(&final_def);

        /* Inject IF NOT EXISTS and replace table name if needed */
        {
            char *p = indexdef;

            if (strncmp(p, "CREATE UNIQUE INDEX ", 20) == 0)
            {
                appendStringInfoString(&final_def, "CREATE UNIQUE INDEX IF NOT EXISTS ");
                p += 20;
            }
            else if (strncmp(p, "CREATE INDEX ", 13) == 0)
            {
                appendStringInfoString(&final_def, "CREATE INDEX IF NOT EXISTS ");
                p += 13;
            }
            else
            {
                appendStringInfoString(&final_def, p);
                p += strlen(p);
            }

            if (strcmp(source_table, target_table) != 0)
            {
                char  search_str[NAMEDATALEN * 2 + 8];
                char *pos;

                snprintf(search_str, sizeof(search_str), " ON %s.%s ",
                         quote_identifier(schema_name),
                         quote_identifier(source_table));

                pos = strstr(p, search_str);
                if (pos != NULL)
                {
                    appendBinaryStringInfo(&final_def, p, pos - p);
                    appendStringInfo(&final_def, " ON %s.%s ",
                                     quote_identifier(schema_name),
                                     quote_identifier(target_table));
                    appendStringInfoString(&final_def, pos + strlen(search_str));
                }
                else
                {
                    appendStringInfoString(&final_def, p);
                }
            }
            else
            {
                appendStringInfoString(&final_def, p);
            }
        }

        if (pgclone_exec_conn(target_conn, final_def.data))
            created++;

        pfree(final_def.data);
        pfree(indexdef);
    }

    PQclear(res);
    pfree(buf.data);

    return created;
}

/* ---------------------------------------------------------------
 * Internal helper: clone constraints for a table from source.
 * --------------------------------------------------------------- */
static int
pgclone_constraints(PGconn *source_conn, PGconn *target_conn,
                      const char *schema_name, const char *source_table,
                      const char *target_table,
                      const CloneOptions *opts)
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
        "AND con.contype != 'n' "  /* NOT NULL is handled by table DDL already */
        "AND con.contype != 'f' "  /* FK constraints handled separately after all tables created */
        "ORDER BY "
        "  CASE contype "
        "    WHEN 'p' THEN 1 "
        "    WHEN 'u' THEN 2 "
        "    WHEN 'x' THEN 3 "  /* EXCLUSION constraints */
        "    WHEN 'c' THEN 4 "
        "    WHEN 'f' THEN 5 "
        "    ELSE 6 "
        "  END, conname",
        quote_literal_cstr(schema_name),
        quote_literal_cstr(source_table));

    res = pgclone_exec(source_conn, buf.data);

    count = PQntuples(res);
    for (i = 0; i < count; i++)
    {
        const char *conname = PQgetvalue(res, i, 0);
        const char *condef  = PQgetvalue(res, i, 2);

        /* Skip if constraint already exists on target */
        {
            StringInfoData chk_buf;
            PGresult      *chk;
            bool           already_exists;

            initStringInfo(&chk_buf);
            appendStringInfo(&chk_buf,
                "SELECT 1 FROM pg_catalog.pg_constraint con "
                "JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "WHERE n.nspname = %s AND c.relname = %s AND con.conname = %s",
                quote_literal_cstr(schema_name),
                quote_literal_cstr(target_table),
                quote_literal_cstr(conname));
            chk = PQexec(target_conn, chk_buf.data);
            already_exists = (PQresultStatus(chk) == PGRES_TUPLES_OK &&
                              PQntuples(chk) > 0);
            PQclear(chk);
            pfree(chk_buf.data);
            if (already_exists)
                continue;
        }

        /*
         * If selective column cloning, skip constraints that reference
         * columns not present in the target table.
         */
        if (opts != NULL && opts->num_columns > 0)
        {
            PGresult       *col_chk;
            StringInfoData  col_buf;
            bool            cols_missing = false;

            initStringInfo(&col_buf);
            appendStringInfo(&col_buf,
                "SELECT a.attname "
                "FROM pg_catalog.pg_constraint con "
                "JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid "
                "  AND a.attnum = ANY(con.conkey) "
                "WHERE n.nspname = %s AND c.relname = %s AND con.conname = %s",
                quote_literal_cstr(schema_name),
                quote_literal_cstr(source_table),
                quote_literal_cstr(conname));

            col_chk = pgclone_exec(source_conn, col_buf.data);
            pfree(col_buf.data);

            if (PQresultStatus(col_chk) == PGRES_TUPLES_OK)
            {
                int ci, cj;
                for (ci = 0; ci < PQntuples(col_chk) && !cols_missing; ci++)
                {
                    const char *col = PQgetvalue(col_chk, ci, 0);
                    bool found = false;
                    for (cj = 0; cj < opts->num_columns; cj++)
                    {
                        if (strcmp(col, opts->columns[cj]) == 0)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        cols_missing = true;
                }
            }
            PQclear(col_chk);

            if (cols_missing)
                continue;
        }

        /*
         * If cloning to a different table name, rename the constraint
         * to avoid conflicts with constraints on other tables.
         * e.g. "simple_test_pkey" -> "simple_test_copy_pkey"
         */
        {
            char       new_conname[NAMEDATALEN * 2];
            const char *effective_conname = conname;

            if (strcmp(source_table, target_table) != 0)
            {
                /* Replace source_table prefix in constraint name */
                size_t src_len = strlen(source_table);
                if (strncmp(conname, source_table, src_len) == 0)
                {
                    snprintf(new_conname, sizeof(new_conname), "%s%s",
                             target_table, conname + src_len);
                    effective_conname = new_conname;
                }
            }

            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "ALTER TABLE %s.%s ADD CONSTRAINT %s %s",
                quote_identifier(schema_name),
                quote_identifier(target_table),
                quote_identifier(effective_conname),
                condef);
        }

        if (pgclone_exec_conn(target_conn, buf.data))
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
pgclone_triggers(PGconn *source_conn, PGconn *target_conn,
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

    res = pgclone_exec(source_conn, buf.data);

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

                if (pgclone_exec_conn(target_conn, new_def.data))
                    created++;
                pfree(new_def.data);
            }
            else
            {
                if (pgclone_exec_conn(target_conn, trigdef))
                    created++;
            }
        }
        else
        {
            if (pgclone_exec_conn(target_conn, trigdef))
                created++;
        }
    }

    PQclear(res);
    pfree(buf.data);

    return created;
}

/* ===============================================================
 * FUNCTION: pgclone_table
 *
 * Overloads:
 *   pgclone_table(conninfo, schema, table, include_data)
 *   pgclone_table(conninfo, schema, table, include_data, target_name)
 *   pgclone_table(conninfo, schema, table, include_data, target_name, options_json)
 *   pgclone_table_ex(conninfo, schema, table, include_data, target_name,
 *                      include_indexes, include_constraints, include_triggers)
 *
 * All variants go through the same C function. Arguments are
 * detected by PG_NARGS() and PG_ARGISNULL().
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_table);

Datum
pgclone_table(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    text       *tablename_t       = PG_GETARG_TEXT_PP(2);
    bool        include_data      = PG_GETARG_BOOL(3);

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);
    char       *table_name        = text_to_cstring(tablename_t);
    char       *target_name;
    CloneOptions opts             = pgclone_default_options();

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
        opts = pgclone_parse_options(options_json);
        pfree(options_json);
    }

    /* Args 5,6,7: boolean overload (8 args via pgclone_table_ex) */
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
    source_conn = pgclone_connect(source_conninfo);

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

    res = pgclone_exec(source_conn, buf.data);

    if (PQntuples(res) == 0)
    {
        PQclear(res);
        PQfinish(source_conn);
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("pgclone: table \"%s.%s\" not found on source",
                        schema_name, table_name)));
    }

    /* Use loopback libpq connection for ALL local operations */
    local_conn = pgclone_connect_local();

    /* ---- Step 1b: Create schema locally ---- */
    {
        resetStringInfo(&buf);
        appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                         quote_identifier(schema_name));
        pgclone_exec_conn(local_conn, buf.data);
    }

    /* ---- Step 1c: Pre-create sequences this table depends on ---- */
    {
        PGresult *seq_res;
        int       si, nseqs;

        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "SELECT s.relname, "
            "       pg_sequence.seqstart, "
            "       pg_sequence.seqincrement, "
            "       pg_sequence.seqmax, "
            "       pg_sequence.seqmin, "
            "       pg_sequence.seqcache, "
            "       pg_sequence.seqcycle, "
            "       pg_sequence.seqtypid::regtype::text "
            "FROM pg_catalog.pg_class t "
            "JOIN pg_catalog.pg_namespace n ON n.oid = t.relnamespace "
            "JOIN pg_catalog.pg_depend dep ON dep.refobjid = t.oid "
            "                              AND dep.deptype = 'a' "
            "                              AND dep.classid = 'pg_catalog.pg_class'::regclass "
            "JOIN pg_catalog.pg_class s ON s.oid = dep.objid AND s.relkind = 'S' "
            "JOIN pg_catalog.pg_sequence ON pg_sequence.seqrelid = s.oid "
            "WHERE n.nspname = %s AND t.relname = %s",
            quote_literal_cstr(schema_name),
            quote_literal_cstr(table_name));

        seq_res = pgclone_exec(source_conn, buf.data);
        nseqs   = PQntuples(seq_res);

        for (si = 0; si < nseqs; si++)
        {
            PGresult *lcres;

            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "CREATE SEQUENCE IF NOT EXISTS %s.%s "
                "AS %s "
                "START WITH %s INCREMENT BY %s "
                "MINVALUE %s MAXVALUE %s CACHE %s %s",
                quote_identifier(schema_name),
                quote_identifier(PQgetvalue(seq_res, si, 0)),
                PQgetvalue(seq_res, si, 7),    /* data type */
                PQgetvalue(seq_res, si, 1),    /* start */
                PQgetvalue(seq_res, si, 2),    /* increment */
                PQgetvalue(seq_res, si, 4),    /* min */
                PQgetvalue(seq_res, si, 3),    /* max */
                PQgetvalue(seq_res, si, 5),    /* cache */
                strcmp(PQgetvalue(seq_res, si, 6), "t") == 0 ? "CYCLE" : "NO CYCLE");

            lcres = PQexec(local_conn, buf.data);
            if (PQresultStatus(lcres) != PGRES_COMMAND_OK)
                ereport(WARNING,
                        (errmsg("pgclone: could not create sequence %s.%s: %s",
                                schema_name, PQgetvalue(seq_res, si, 0),
                                PQerrorMessage(local_conn))));
            PQclear(lcres);
        }
        PQclear(seq_res);
    }

    /* ---- Step 2: Create table via loopback ---- */
    {
        PGresult *local_res;
        char     *ddl;

        ddl = pstrdup(PQgetvalue(res, 0, 0));
        PQclear(res);

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
                     errmsg("pgclone: failed to create table locally: %s",
                            ddl_errmsg)));
        }
        PQclear(local_res);
        pfree(ddl);
    }

    /* ---- Step 3: Copy data if requested ---- */
    if (include_data)
    {
        int64 row_count;

        row_count = pgclone_copy_data(source_conn, local_conn,
                                        schema_name, table_name, target_name,
                                        &opts);

        elog(DEBUG1, "pgclone: copied %ld rows into %s.%s using COPY protocol",
             (long) row_count, schema_name, target_name);
    }

    /* ---- Step 4: Clone constraints if enabled ---- */
    if (opts.include_constraints)
    {
        int con_count = pgclone_constraints(source_conn, local_conn,
                                              schema_name, table_name, target_name,
                                              &opts);
        if (con_count > 0)
            elog(DEBUG1, "pgclone: cloned %d constraints for %s.%s",
                 con_count, schema_name, target_name);
    }

    /* ---- Step 5: Clone indexes if enabled ---- */
    if (opts.include_indexes)
    {
        int idx_count = pgclone_indexes(source_conn, local_conn,
                                          schema_name, table_name, target_name,
                                          &opts);
        if (idx_count > 0)
            elog(DEBUG1, "pgclone: cloned %d indexes for %s.%s",
                 idx_count, schema_name, target_name);
    }

    /* ---- Step 6: Clone triggers if enabled ---- */
    if (opts.include_triggers)
    {
        int trig_count = pgclone_triggers(source_conn, local_conn,
                                            schema_name, table_name, target_name);
        if (trig_count > 0)
            elog(DEBUG1, "pgclone: cloned %d triggers for %s.%s",
                 trig_count, schema_name, target_name);
    }

    PQfinish(local_conn);
    PQfinish(source_conn);

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgclone_table_ex
 *
 * Boolean overload: separate params for indexes, constraints, triggers
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_table_ex);

Datum
pgclone_table_ex(PG_FUNCTION_ARGS)
{
    /* Just forward to pgclone_table — same C function handles both */
    return pgclone_table(fcinfo);
}

/* ===============================================================
 * FUNCTION: pgclone_schema(source_conninfo, schema, include_data [, options])
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_schema);

Datum
pgclone_schema(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    bool        include_data      = PG_GETARG_BOOL(2);
    CloneOptions opts             = pgclone_default_options();

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
        opts = pgclone_parse_options(options_json);
        pfree(options_json);
    }

    /* Arg 3,4,5: boolean overload (6 args via pgclone_schema_ex) */
    if (PG_NARGS() == 6)
    {
        if (!PG_ARGISNULL(3))
            opts.include_indexes = PG_GETARG_BOOL(3);
        if (!PG_ARGISNULL(4))
            opts.include_constraints = PG_GETARG_BOOL(4);
        if (!PG_ARGISNULL(5))
            opts.include_triggers = PG_GETARG_BOOL(5);
    }

    source_conn = pgclone_connect(source_conninfo);
    local_conn = pgclone_connect_local();

    /* ---- Step 1: Create schema ---- */
    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                     quote_identifier(schema_name));
    pgclone_exec_conn(local_conn, buf.data);

    /* ---- Step 2: Clone all functions/procedures ---- */
    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT pg_get_functiondef(p.oid) AS funcdef "
        "FROM pg_catalog.pg_proc p "
        "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
        "WHERE n.nspname = %s",
        quote_literal_cstr(schema_name));

    res = pgclone_exec(source_conn, buf.data);

    for (i = 0; i < PQntuples(res); i++)
    {
        pgclone_exec_conn(local_conn, PQgetvalue(res, i, 0));
    }
    elog(DEBUG1, "pgclone: cloned %d functions from schema %s",
         PQntuples(res), schema_name);
    PQclear(res);

    /* ---- Step 3: Clone sequences ---- */
    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT sequence_name, data_type, start_value, increment, "
        "minimum_value, maximum_value, cycle_option "
        "FROM information_schema.sequences "
        "WHERE sequence_schema = %s",
        quote_literal_cstr(schema_name));

    res = pgclone_exec(source_conn, buf.data);

    for (i = 0; i < PQntuples(res); i++)
    {
        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "CREATE SEQUENCE IF NOT EXISTS %s.%s "
            "AS %s START WITH %s INCREMENT BY %s "
            "MINVALUE %s MAXVALUE %s %s",
            quote_identifier(schema_name),
            quote_identifier(PQgetvalue(res, i, 0)),
            PQgetvalue(res, i, 1),
            PQgetvalue(res, i, 2),
            PQgetvalue(res, i, 3),
            PQgetvalue(res, i, 4),
            PQgetvalue(res, i, 5),
            strcmp(PQgetvalue(res, i, 6), "YES") == 0 ? "CYCLE" : "NO CYCLE");
        pgclone_exec_conn(local_conn, buf.data);
    }
    elog(DEBUG1, "pgclone: cloned %d sequences from schema %s",
         PQntuples(res), schema_name);
    PQclear(res);

    PQfinish(local_conn);

    /* ---- Step 4: Clone tables ---- */
    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT tablename FROM pg_catalog.pg_tables "
        "WHERE schemaname = %s ORDER BY tablename",
        quote_literal_cstr(schema_name));

    res = pgclone_exec(source_conn, buf.data);

    ntables = PQntuples(res);

    if (ntables > 0)
    {
        table_names = palloc(sizeof(char *) * ntables);
        for (i = 0; i < ntables; i++)
            table_names[i] = pstrdup(PQgetvalue(res, i, 0));
    }
    PQclear(res);
    PQfinish(source_conn);

    /* Build options JSON to pass through to pgclone_table */
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
            result = DirectFunctionCall6(pgclone_table,
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

    elog(DEBUG1, "pgclone: cloned %d tables from schema %s",
         ntables, schema_name);

    /* ---- Step 5: Retry FK constraints if constraints enabled ---- */
    if (opts.include_constraints)
    {
        PGconn *src_retry  = pgclone_connect(source_conninfo);
        PGconn *lcl_retry  = pgclone_connect_local();
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

            res = pgclone_exec(src_retry, buf.data);

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

                    if (pgclone_exec_conn(lcl_retry, buf.data))
                        fk_created++;
                }
            }

            PQclear(res);
        }

        if (fk_created > 0)
            elog(DEBUG1, "pgclone: FK retry pass: ensured %d foreign key constraints in schema %s",
                 fk_created, schema_name);

        PQfinish(lcl_retry);
        PQfinish(src_retry);
    }

    /* ---- Step 6: Clone views ---- */
    {
        PGconn *src_views = pgclone_connect(source_conninfo);
        PGconn *lcl_views = pgclone_connect_local();

        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "SELECT table_name, view_definition "
            "FROM information_schema.views "
            "WHERE table_schema = %s",
            quote_literal_cstr(schema_name));

        res = pgclone_exec(src_views, buf.data);

        for (i = 0; i < PQntuples(res); i++)
        {
            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "CREATE OR REPLACE VIEW %s.%s AS %s",
                quote_identifier(schema_name),
                quote_identifier(PQgetvalue(res, i, 0)),
                PQgetvalue(res, i, 1));
            pgclone_exec_conn(lcl_views, buf.data);
        }
        elog(DEBUG1, "pgclone: cloned %d views from schema %s",
             PQntuples(res), schema_name);
        PQclear(res);

        /* ---- Step 7: Clone materialized views ---- */
        if (opts.include_matviews)
        {
            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "SELECT c.relname AS matview_name, "
                "pg_get_viewdef(c.oid, true) AS matview_def "
                "FROM pg_catalog.pg_class c "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "WHERE n.nspname = %s AND c.relkind = 'm' "
                "ORDER BY c.relname",
                quote_literal_cstr(schema_name));

            res = pgclone_exec(src_views, buf.data);

            for (i = 0; i < PQntuples(res); i++)
            {
                const char *mv_name = PQgetvalue(res, i, 0);
                const char *mv_def  = PQgetvalue(res, i, 1);

                /* Create the materialized view — strip trailing semicolon if present */
                {
                    char *mv_def_clean = pstrdup(mv_def);
                    size_t mv_len = strlen(mv_def_clean);
                    while (mv_len > 0 &&
                           (mv_def_clean[mv_len - 1] == ';' ||
                            mv_def_clean[mv_len - 1] == ' ' ||
                            mv_def_clean[mv_len - 1] == '\n'))
                        mv_def_clean[--mv_len] = '\0';

                    resetStringInfo(&buf);
                    appendStringInfo(&buf,
                        "CREATE MATERIALIZED VIEW IF NOT EXISTS %s.%s AS %s WITH DATA",
                        quote_identifier(schema_name),
                        quote_identifier(mv_name),
                        mv_def_clean);
                    pgclone_exec_conn(lcl_views, buf.data);
                    pfree(mv_def_clean);
                }

                /* Clone indexes on materialized view */
                if (opts.include_indexes)
                {
                    PGresult *idx_res;
                    int       idx_i;

                    resetStringInfo(&buf);
                    appendStringInfo(&buf,
                        "SELECT pg_get_indexdef(i.indexrelid) "
                        "FROM pg_catalog.pg_index i "
                        "JOIN pg_catalog.pg_class c ON c.oid = i.indrelid "
                        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                        "WHERE n.nspname = %s AND c.relname = %s",
                        quote_literal_cstr(schema_name),
                        quote_literal_cstr(mv_name));

                    idx_res = pgclone_exec(src_views, buf.data);
                    for (idx_i = 0; idx_i < PQntuples(idx_res); idx_i++)
                        pgclone_exec_conn(lcl_views, PQgetvalue(idx_res, idx_i, 0));
                    PQclear(idx_res);
                }
            }

            elog(DEBUG1, "pgclone: cloned %d materialized views from schema %s",
                 PQntuples(res), schema_name);
            PQclear(res);
        }

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

/* pgclone_schema_ex — boolean overload for schema */
PG_FUNCTION_INFO_V1(pgclone_schema_ex);

Datum
pgclone_schema_ex(PG_FUNCTION_ARGS)
{
    return pgclone_schema(fcinfo);
}

/* ===============================================================
 * FUNCTION: pgclone_functions(source_conninfo, schema)
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_functions);

Datum
pgclone_functions(PG_FUNCTION_ARGS)
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

    source_conn = pgclone_connect(source_conninfo);
    local_conn  = pgclone_connect_local();

    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                     quote_identifier(schema_name));
    pgclone_exec_conn(local_conn, buf.data);

    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT pg_get_functiondef(p.oid) AS funcdef "
        "FROM pg_catalog.pg_proc p "
        "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
        "WHERE n.nspname = %s",
        quote_literal_cstr(schema_name));

    res = pgclone_exec(source_conn, buf.data);

    count = PQntuples(res);
    for (i = 0; i < count; i++)
    {
        pgclone_exec_conn(local_conn, PQgetvalue(res, i, 0));
    }

    PQclear(res);
    PQfinish(local_conn);
    PQfinish(source_conn);

    elog(DEBUG1, "pgclone: successfully cloned %d functions from %s",
         count, schema_name);

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgclone_database(source_conninfo, include_data [, options])
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_database);

Datum
pgclone_database(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    bool        include_data      = PG_GETARG_BOOL(1);
    CloneOptions opts             = pgclone_default_options();

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);

    PGconn     *source_conn;
    PGresult   *res;
    int         i, nschemas;
    char      **schema_names;

    /* Arg 2: JSON options (optional) */
    if (PG_NARGS() >= 3 && !PG_ARGISNULL(2))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(2));
        opts = pgclone_parse_options(options_json);
        pfree(options_json);
    }

    source_conn = pgclone_connect(source_conninfo);

    res = pgclone_exec(source_conn,
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

            elog(DEBUG1, "pgclone: cloning schema %s (%d/%d)",
                 schema_names[i], i + 1, nschemas);

            result = DirectFunctionCall4(pgclone_schema,
                        CStringGetTextDatum(source_conninfo),
                        CStringGetTextDatum(schema_names[i]),
                        BoolGetDatum(include_data),
                        CStringGetTextDatum(opts_json.data));
            (void) result;
        }

        pfree(opts_json.data);
    }

    ereport(NOTICE,
            (errmsg("pgclone: database clone complete — %d schemas cloned",
                    nschemas)));

    for (i = 0; i < nschemas; i++)
        pfree(schema_names[i]);
    pfree(schema_names);

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgclone_database_create(source_conninfo, target_dbname
 *              [, include_data [, options]])
 *
 * Creates the target database locally if it does not exist,
 * installs the pgclone extension in it, then delegates to
 * pgclone_database() running inside the target database so that
 * all schemas, tables, functions, etc. are cloned from the
 * remote source into the freshly created local database.
 *
 * Must be called from any local database (typically "postgres").
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_database_create);

Datum
pgclone_database_create(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *target_dbname_t   = PG_GETARG_TEXT_PP(1);
    bool        include_data      = true;
    char       *options_json      = NULL;

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *target_dbname     = text_to_cstring(target_dbname_t);

    PGconn     *admin_conn;       /* connection to local postgres DB */
    PGconn     *target_conn;      /* connection to local target DB   */
    PGresult   *res;
    StringInfoData buf;
    const char *port;

    /* Optional arg 2: include_data */
    if (PG_NARGS() >= 3 && !PG_ARGISNULL(2))
        include_data = PG_GETARG_BOOL(2);

    /* Optional arg 3: JSON options */
    if (PG_NARGS() >= 4 && !PG_ARGISNULL(3))
        options_json = text_to_cstring(PG_GETARG_TEXT_PP(3));

    /* Validate target_dbname: must be a simple identifier */
    {
        int ci;
        for (ci = 0; target_dbname[ci] != '\0'; ci++)
        {
            char ch = target_dbname[ci];
            if (!((ch >= 'a' && ch <= 'z') ||
                  (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') ||
                  ch == '_'))
            {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("pgclone: invalid database name: %s", target_dbname)));
            }
        }
        if (ci == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("pgclone: database name cannot be empty")));
    }

    port = GetConfigOption("port", false, false);

    /* ---- Step 1: Connect to local "postgres" DB ---- */
    initStringInfo(&buf);
    appendStringInfo(&buf, "dbname=%s port=%s",
                     quote_literal_cstr("postgres"),
                     port ? port : "5432");

    admin_conn = PQconnectdb(buf.data);
    if (PQstatus(admin_conn) != CONNECTION_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(admin_conn));
        PQfinish(admin_conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgclone: could not connect to postgres DB: %s",
                        errmsg_str)));
    }

    /* ---- Step 2: Check if target database exists ---- */
    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT 1 FROM pg_catalog.pg_database WHERE datname = %s",
        quote_literal_cstr(target_dbname));

    res = PQexec(admin_conn, buf.data);
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(admin_conn));
        PQclear(res);
        PQfinish(admin_conn);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: could not check database existence: %s",
                        errmsg_str)));
    }

    if (PQntuples(res) == 0)
    {
        PQclear(res);

        /* CREATE DATABASE — cannot run inside a transaction */
        resetStringInfo(&buf);
        appendStringInfo(&buf, "CREATE DATABASE %s",
                         quote_identifier(target_dbname));

        res = PQexec(admin_conn, buf.data);
        if (PQresultStatus(res) != PGRES_COMMAND_OK)
        {
            char *errmsg_str = pstrdup(PQerrorMessage(admin_conn));
            PQclear(res);
            PQfinish(admin_conn);
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("pgclone: could not create database %s: %s",
                            target_dbname, errmsg_str)));
        }
        PQclear(res);

        ereport(NOTICE,
                (errmsg("pgclone: created database %s", target_dbname)));
    }
    else
    {
        PQclear(res);
        ereport(NOTICE,
                (errmsg("pgclone: database %s already exists, cloning into it",
                        target_dbname)));
    }

    PQfinish(admin_conn);

    /* ---- Step 3: Connect to target database ---- */
    resetStringInfo(&buf);
    appendStringInfo(&buf, "dbname=%s port=%s",
                     quote_literal_cstr(target_dbname),
                     port ? port : "5432");

    target_conn = PQconnectdb(buf.data);
    if (PQstatus(target_conn) != CONNECTION_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(target_conn));
        PQfinish(target_conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgclone: could not connect to target database %s: %s",
                        target_dbname, errmsg_str)));
    }

    /* ---- Step 4: Install pgclone extension in target DB ---- */
    res = PQexec(target_conn, "CREATE EXTENSION IF NOT EXISTS pgclone");
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(target_conn));
        PQclear(res);
        PQfinish(target_conn);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: could not install pgclone in %s: %s",
                        target_dbname, errmsg_str)));
    }
    PQclear(res);

    ereport(NOTICE,
            (errmsg("pgclone: pgclone extension ready in %s", target_dbname)));

    /* ---- Step 5: Execute pgclone_database() inside target DB ---- */
    resetStringInfo(&buf);

    if (options_json != NULL)
    {
        /* 3-arg: pgclone_database(conninfo, include_data, options) */
        appendStringInfo(&buf,
            "SELECT pgclone_database(%s, %s, %s)",
            quote_literal_cstr(source_conninfo),
            include_data ? "true" : "false",
            quote_literal_cstr(options_json));
    }
    else
    {
        /* 2-arg: pgclone_database(conninfo, include_data) */
        appendStringInfo(&buf,
            "SELECT pgclone_database(%s, %s)",
            quote_literal_cstr(source_conninfo),
            include_data ? "true" : "false");
    }

    ereport(NOTICE,
            (errmsg("pgclone: starting database clone from source into %s ...",
                    target_dbname)));

    res = PQexec(target_conn, buf.data);
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(target_conn));
        PQclear(res);
        PQfinish(target_conn);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: database clone failed in %s: %s",
                        target_dbname, errmsg_str)));
    }
    PQclear(res);
    PQfinish(target_conn);

    pfree(buf.data);
    pfree(source_conninfo);
    pfree(target_dbname);
    if (options_json)
        pfree(options_json);

    ereport(NOTICE,
            (errmsg("pgclone: database clone complete — target database %s ready",
                    text_to_cstring(target_dbname_t))));

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgclone_version()
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_version);

Datum
pgclone_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("pgclone 2.1.2"));
}

/* ===============================================================
 * _PG_init — called when the shared library is loaded.
 * Registers shared memory for job tracking.
 * Required: shared_preload_libraries = 'pgclone'
 * =============================================================== */
void _PG_init(void);

void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
        return;

    pgclone_shmem_init();
}

/* ===============================================================
 * ASYNC CLONE FUNCTIONS
 *
 * Submit clone jobs to background workers for non-blocking
 * operations with progress tracking, cancel, and resume.
 * =============================================================== */

/* ===============================================================
 * FUNCTION: pgclone_table_async(conninfo, schema, table,
 *              include_data [, target_name [, options_json]])
 *
 * Returns job_id (INTEGER).
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_table_async);

Datum
pgclone_table_async(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    text       *tablename_t       = PG_GETARG_TEXT_PP(2);
    bool        include_data      = PG_GETARG_BOOL(3);

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);
    char       *table_name        = text_to_cstring(tablename_t);
    char       *target_name       = table_name;
    CloneOptions opts             = pgclone_default_options();
    PgcloneConflictStrategy conflict = PGCLONE_CONFLICT_ERROR;

    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;
    PgcloneJob    *job;
    int             job_id;

    if (PG_NARGS() >= 5 && !PG_ARGISNULL(4))
        target_name = text_to_cstring(PG_GETARG_TEXT_PP(4));

    if (PG_NARGS() >= 6 && !PG_ARGISNULL(5))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(5));
        opts = pgclone_parse_options(options_json);

        if (strstr(options_json, "\"conflict\": \"skip\"") ||
            strstr(options_json, "\"conflict\":\"skip\""))
            conflict = PGCLONE_CONFLICT_SKIP;
        else if (strstr(options_json, "\"conflict\": \"replace\"") ||
                 strstr(options_json, "\"conflict\":\"replace\""))
            conflict = PGCLONE_CONFLICT_REPLACE;
        else if (strstr(options_json, "\"conflict\": \"rename\"") ||
                 strstr(options_json, "\"conflict\":\"rename\""))
            conflict = PGCLONE_CONFLICT_RENAME;

        pfree(options_json);
    }

    if (!pgclone_state)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pgclone: shared memory not initialized"),
                 errhint("Add pgclone to shared_preload_libraries in postgresql.conf")));

    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

    job = find_free_slot();
    if (!job)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                 errmsg("pgclone: no free job slots (max %d)", PGCLONE_MAX_JOBS)));
    }

    job_id = pgclone_state->next_job_id++;
    memset(job, 0, sizeof(PgcloneJob));
    job->job_id = job_id;
    job->status = PGCLONE_JOB_PENDING;
    job->op_type = PGCLONE_OP_TABLE;
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

    LWLockRelease(pgclone_state->lock);

    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "pgclone: job %d (%s.%s)",
             job_id, schema_name, table_name);
    snprintf(worker.bgw_type, BGW_MAXLEN, "pgclone worker");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgclone");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgclone_bgw_main");
    worker.bgw_main_arg = Int32GetDatum(job_id);
    worker.bgw_notify_pid = MyProcPid;

    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
    {
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        job->status = PGCLONE_JOB_FREE;
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: could not register background worker")));
    }

    PG_RETURN_INT32(job_id);
}

/* ===============================================================
 * FUNCTION: pgclone_schema_async(conninfo, schema, include_data
 *              [, options_json])
 *
 * When options contains "parallel": N (N > 1), launches N background
 * workers that each clone a subset of tables concurrently.
 * Without parallel, launches a single worker (as before).
 *
 * Returns the parent job_id. Child jobs are visible via pgclone_jobs().
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_schema_async);

Datum
pgclone_schema_async(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    bool        include_data      = PG_GETARG_BOOL(2);
    CloneOptions opts             = pgclone_default_options();
    PgcloneConflictStrategy conflict = PGCLONE_CONFLICT_ERROR;

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);

    PgcloneJob    *job;
    int             job_id;

    if (PG_NARGS() >= 4 && !PG_ARGISNULL(3))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(3));
        opts = pgclone_parse_options(options_json);

        if (strstr(options_json, "\"conflict\": \"skip\"") ||
            strstr(options_json, "\"conflict\":\"skip\""))
            conflict = PGCLONE_CONFLICT_SKIP;
        else if (strstr(options_json, "\"conflict\": \"replace\"") ||
                 strstr(options_json, "\"conflict\":\"replace\""))
            conflict = PGCLONE_CONFLICT_REPLACE;
        else if (strstr(options_json, "\"conflict\": \"rename\"") ||
                 strstr(options_json, "\"conflict\":\"rename\""))
            conflict = PGCLONE_CONFLICT_RENAME;

        pfree(options_json);
    }

    if (!pgclone_state)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pgclone: shared memory not initialized"),
                 errhint("Add pgclone to shared_preload_libraries")));

    if (opts.parallel_workers > 1)
    {
        /*
         * PARALLEL MODE: Get table list, then launch N workers
         * each handling a subset of tables.
         */
        PGconn     *source_conn;
        PGresult   *table_res;
        StringInfoData qbuf;
        int         ntables, tables_per_worker, wi;
        int         parent_job_id;
        int         workers_launched = 0;

        /* First create schema + sequences + functions via loopback */
        {
            PGconn     *local_conn = pgclone_connect_local();
            PGresult   *func_res;
            StringInfoData fbuf;

            initStringInfo(&fbuf);
            appendStringInfo(&fbuf, "CREATE SCHEMA IF NOT EXISTS %s",
                             quote_identifier(schema_name));
            pgclone_exec_conn(local_conn, fbuf.data);

            PQfinish(local_conn);
            pfree(fbuf.data);
        }

        /* Get table list from source */
        source_conn = pgclone_connect(source_conninfo);

        initStringInfo(&qbuf);
        appendStringInfo(&qbuf,
            "SELECT tablename FROM pg_catalog.pg_tables "
            "WHERE schemaname = %s ORDER BY tablename",
            quote_literal_cstr(schema_name));

        table_res = pgclone_exec(source_conn, qbuf.data);
        ntables = PQntuples(table_res);

        PQfinish(source_conn);

        if (ntables == 0)
        {
            PQclear(table_res);
            pfree(qbuf.data);
            elog(DEBUG1, "pgclone: no tables found in schema %s", schema_name);
            PG_RETURN_INT32(0);
        }

        /* Allocate parent job for tracking */
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

        job = find_free_slot();
        if (!job)
        {
            LWLockRelease(pgclone_state->lock);
            PQclear(table_res);
            pfree(qbuf.data);
            ereport(ERROR, (errmsg("pgclone: no free job slots")));
        }

        parent_job_id = pgclone_state->next_job_id++;
        memset(job, 0, sizeof(PgcloneJob));
        job->job_id = parent_job_id;
        job->status = PGCLONE_JOB_RUNNING;
        job->op_type = PGCLONE_OP_SCHEMA;
        job->database_oid = MyDatabaseId;
        job->total_tables = ntables;
        job->parallel_workers = opts.parallel_workers;
        job->start_time = GetCurrentTimestamp();
        strlcpy(job->source_conninfo, source_conninfo, sizeof(job->source_conninfo));
        strlcpy(job->schema_name, schema_name, NAMEDATALEN);
        strlcpy(job->current_phase, "launching parallel workers", 64);

        LWLockRelease(pgclone_state->lock);

        /* Launch workers — each gets a subset of tables */
        tables_per_worker = (ntables + opts.parallel_workers - 1) / opts.parallel_workers;

        for (wi = 0; wi < opts.parallel_workers && wi * tables_per_worker < ntables; wi++)
        {
            int start_idx = wi * tables_per_worker;
            int end_idx = start_idx + tables_per_worker;
            int ti;
            BackgroundWorker worker;
            BackgroundWorkerHandle *handle;
            PgcloneJob *child_job;
            int child_job_id;

            if (end_idx > ntables)
                end_idx = ntables;

            /* For each table in this worker's range, create a table-level job */
            for (ti = start_idx; ti < end_idx; ti++)
            {
                const char *tname = PQgetvalue(table_res, ti, 0);

                LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

                child_job = find_free_slot();
                if (!child_job)
                {
                    LWLockRelease(pgclone_state->lock);
                    ereport(WARNING,
                            (errmsg("pgclone: no free job slot for table %s, skipping", tname)));
                    continue;
                }

                child_job_id = pgclone_state->next_job_id++;
                memset(child_job, 0, sizeof(PgcloneJob));
                child_job->job_id = child_job_id;
                child_job->status = PGCLONE_JOB_PENDING;
                child_job->op_type = PGCLONE_OP_TABLE;
                child_job->database_oid = MyDatabaseId;
                child_job->include_data = include_data;
                child_job->include_indexes = opts.include_indexes;
                child_job->include_constraints = opts.include_constraints;
                child_job->include_triggers = opts.include_triggers;
                child_job->conflict_strategy = conflict;
                child_job->resumable = true;

                strlcpy(child_job->source_conninfo, source_conninfo, sizeof(child_job->source_conninfo));
                strlcpy(child_job->schema_name, schema_name, NAMEDATALEN);
                strlcpy(child_job->table_name, tname, NAMEDATALEN);
                strlcpy(child_job->target_name, tname, NAMEDATALEN);

                LWLockRelease(pgclone_state->lock);

                /* Launch bgworker for this table */
                memset(&worker, 0, sizeof(BackgroundWorker));
                snprintf(worker.bgw_name, BGW_MAXLEN,
                         "pgclone: %s.%s (parallel job %d, worker %d)",
                         schema_name, tname, parent_job_id, wi);
                snprintf(worker.bgw_type, BGW_MAXLEN, "pgclone worker");
                worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
                worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
                worker.bgw_restart_time = BGW_NEVER_RESTART;
                snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgclone");
                snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgclone_bgw_main");
                worker.bgw_main_arg = Int32GetDatum(child_job_id);
                worker.bgw_notify_pid = MyProcPid;

                if (RegisterDynamicBackgroundWorker(&worker, &handle))
                    workers_launched++;
                else
                    ereport(WARNING,
                            (errmsg("pgclone: could not launch worker for table %s", tname)));
            }
        }

        PQclear(table_res);
        pfree(qbuf.data);

        /* Update parent job */
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        snprintf(pgclone_state->jobs[0].current_phase, 64,
                 "%d parallel workers launched", workers_launched);

        /* Find parent job to update */
        {
            PgcloneJob *pj = find_job(parent_job_id);
            if (pj)
                snprintf(pj->current_phase, 64,
                         "%d parallel workers for %d tables", workers_launched, ntables);
        }
        LWLockRelease(pgclone_state->lock);

        ereport(NOTICE,
                (errmsg("pgclone: launched %d parallel workers for %d tables in schema %s (parent job %d)",
                        workers_launched, ntables, schema_name, parent_job_id)));

        PG_RETURN_INT32(parent_job_id);
    }
    else
    {
        /*
         * SEQUENTIAL MODE: Single worker for whole schema (original behavior)
         */
        BackgroundWorker worker;
        BackgroundWorkerHandle *handle;

        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

        job = find_free_slot();
        if (!job)
        {
            LWLockRelease(pgclone_state->lock);
            ereport(ERROR, (errmsg("pgclone: no free job slots")));
        }

        job_id = pgclone_state->next_job_id++;
        memset(job, 0, sizeof(PgcloneJob));
        job->job_id = job_id;
        job->status = PGCLONE_JOB_PENDING;
        job->op_type = PGCLONE_OP_SCHEMA;
        job->database_oid = MyDatabaseId;

        strlcpy(job->source_conninfo, source_conninfo, sizeof(job->source_conninfo));
        strlcpy(job->schema_name, schema_name, NAMEDATALEN);

        job->include_data        = include_data;
        job->include_indexes     = opts.include_indexes;
        job->include_constraints = opts.include_constraints;
        job->include_triggers    = opts.include_triggers;
        job->conflict_strategy   = conflict;
        job->resumable           = true;

        LWLockRelease(pgclone_state->lock);

        memset(&worker, 0, sizeof(BackgroundWorker));
        snprintf(worker.bgw_name, BGW_MAXLEN, "pgclone: schema %s (job %d)",
                 schema_name, job_id);
        snprintf(worker.bgw_type, BGW_MAXLEN, "pgclone worker");
        worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
        worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
        worker.bgw_restart_time = BGW_NEVER_RESTART;
        snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgclone");
        snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgclone_bgw_main");
        worker.bgw_main_arg = Int32GetDatum(job_id);
        worker.bgw_notify_pid = MyProcPid;

        if (!RegisterDynamicBackgroundWorker(&worker, &handle))
        {
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            job->status = PGCLONE_JOB_FREE;
            LWLockRelease(pgclone_state->lock);
            ereport(ERROR, (errmsg("pgclone: could not register background worker")));
        }

        PG_RETURN_INT32(job_id);
    }
}

/* ===============================================================
 * FUNCTION: pgclone_progress(job_id) — returns JSON
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_progress);

Datum
pgclone_progress(PG_FUNCTION_ARGS)
{
    int             job_id = PG_GETARG_INT32(0);
    PgcloneJob    *job;
    StringInfoData  result;
    const char     *status_str;

    if (!pgclone_state)
        ereport(ERROR, (errmsg("pgclone: shared memory not initialized")));

    LWLockAcquire(pgclone_state->lock, LW_SHARED);
    job = find_job(job_id);

    if (!job)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: job %d not found", job_id)));
    }

    switch (job->status)
    {
        case PGCLONE_JOB_PENDING:   status_str = "pending";   break;
        case PGCLONE_JOB_RUNNING:   status_str = "running";   break;
        case PGCLONE_JOB_COMPLETED: status_str = "completed"; break;
        case PGCLONE_JOB_FAILED:    status_str = "failed";    break;
        case PGCLONE_JOB_CANCELLED: status_str = "cancelled"; break;
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

    if (job->status == PGCLONE_JOB_FAILED)
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
    LWLockRelease(pgclone_state->lock);

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/* ===============================================================
 * FUNCTION: pgclone_cancel(job_id)
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_cancel);

Datum
pgclone_cancel(PG_FUNCTION_ARGS)
{
    int             job_id = PG_GETARG_INT32(0);
    PgcloneJob    *job;

    if (!pgclone_state)
        ereport(ERROR, (errmsg("pgclone: shared memory not initialized")));

    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
    job = find_job(job_id);

    if (!job)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: job %d not found", job_id)));
    }

    if (job->status == PGCLONE_JOB_RUNNING ||
        job->status == PGCLONE_JOB_PENDING)
    {
        job->status = PGCLONE_JOB_CANCELLED;
        job->end_time = GetCurrentTimestamp();
        strlcpy(job->current_phase, "cancelled", 64);
    }

    LWLockRelease(pgclone_state->lock);

    PG_RETURN_TEXT_P(cstring_to_text("cancelled"));
}

/* ===============================================================
 * FUNCTION: pgclone_resume(job_id) — returns new job_id
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_resume);

Datum
pgclone_resume(PG_FUNCTION_ARGS)
{
    int             old_job_id = PG_GETARG_INT32(0);
    PgcloneJob    *old_job, *new_job;
    int             new_job_id;
    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;

    if (!pgclone_state)
        ereport(ERROR, (errmsg("pgclone: shared memory not initialized")));

    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

    old_job = find_job(old_job_id);
    if (!old_job)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: job %d not found", old_job_id)));
    }

    if (old_job->status != PGCLONE_JOB_FAILED &&
        old_job->status != PGCLONE_JOB_CANCELLED)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: job %d is not resumable", old_job_id)));
    }

    new_job = find_free_slot();
    if (!new_job)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: no free job slots")));
    }

    new_job_id = pgclone_state->next_job_id++;
    memcpy(new_job, old_job, sizeof(PgcloneJob));
    new_job->job_id = new_job_id;
    new_job->status = PGCLONE_JOB_PENDING;
    new_job->worker_pid = 0;
    new_job->start_time = 0;
    new_job->end_time = 0;
    new_job->error_message[0] = '\0';
    /* resume_checkpoint preserved — bgworker will skip past it */

    old_job->status = PGCLONE_JOB_FREE;

    LWLockRelease(pgclone_state->lock);

    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "pgclone: resume job %d", new_job_id);
    snprintf(worker.bgw_type, BGW_MAXLEN, "pgclone worker");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgclone");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgclone_bgw_main");
    worker.bgw_main_arg = Int32GetDatum(new_job_id);
    worker.bgw_notify_pid = MyProcPid;

    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
    {
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        new_job->status = PGCLONE_JOB_FREE;
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: could not register background worker")));
    }

    PG_RETURN_INT32(new_job_id);
}

/* ===============================================================
 * FUNCTION: pgclone_jobs() — returns JSON array of all jobs
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_jobs);

Datum
pgclone_jobs(PG_FUNCTION_ARGS)
{
    StringInfoData  result;
    int             i;
    bool            first = true;
    const char     *status_str;

    if (!pgclone_state)
        ereport(ERROR, (errmsg("pgclone: shared memory not initialized")));

    initStringInfo(&result);
    appendStringInfoChar(&result, '[');

    LWLockAcquire(pgclone_state->lock, LW_SHARED);

    for (i = 0; i < PGCLONE_MAX_JOBS; i++)
    {
        PgcloneJob *j = &pgclone_state->jobs[i];

        if (j->status == PGCLONE_JOB_FREE)
            continue;

        if (!first)
            appendStringInfoChar(&result, ',');
        first = false;

        switch (j->status)
        {
            case PGCLONE_JOB_PENDING:   status_str = "pending";   break;
            case PGCLONE_JOB_RUNNING:   status_str = "running";   break;
            case PGCLONE_JOB_COMPLETED: status_str = "completed"; break;
            case PGCLONE_JOB_FAILED:    status_str = "failed";    break;
            case PGCLONE_JOB_CANCELLED: status_str = "cancelled"; break;
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

    LWLockRelease(pgclone_state->lock);
    appendStringInfoChar(&result, ']');

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/* ===============================================================
 * FUNCTION: pgclone_progress_view() — SET-RETURNING FUNCTION
 *
 * Returns one row per active/recent job from shared memory.
 * Designed to back the pgclone_jobs_view VIEW.
 *
 * Columns:
 *   job_id              INTEGER
 *   status              TEXT
 *   op_type             TEXT
 *   schema_name         TEXT
 *   table_name          TEXT
 *   current_phase       TEXT
 *   current_table       TEXT
 *   tables_total        BIGINT
 *   tables_completed    BIGINT
 *   rows_copied         BIGINT
 *   bytes_copied        BIGINT
 *   elapsed_ms          BIGINT
 *   start_time          TIMESTAMPTZ
 *   end_time            TIMESTAMPTZ
 *   error_message       TEXT
 *   pct_complete        DOUBLE PRECISION
 *   progress_bar        TEXT
 *   elapsed_time        TEXT        (human-readable: HH:MM:SS)
 * =============================================================== */
#define PGCLONE_VIEW_COLS 18

PG_FUNCTION_INFO_V1(pgclone_progress_view);

Datum
pgclone_progress_view(PG_FUNCTION_ARGS)
{
    FuncCallContext    *funcctx;
    PgcloneJob         *job;
    int                 slot_index;

    if (!pgclone_state)
        ereport(ERROR, (errmsg("pgclone: shared memory not initialized — "
                               "add pgclone to shared_preload_libraries")));

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext   oldctx;
        TupleDesc       tupdesc;

        funcctx = SRF_FIRSTCALL_INIT();
        oldctx  = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        tupdesc = CreateTemplateTupleDesc(PGCLONE_VIEW_COLS);
        TupleDescInitEntry(tupdesc,  1, "job_id",           INT4OID,   -1, 0);
        TupleDescInitEntry(tupdesc,  2, "status",           TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  3, "op_type",          TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  4, "schema_name",      TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  5, "table_name",       TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  6, "current_phase",    TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  7, "current_table",    TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  8, "tables_total",     INT8OID,   -1, 0);
        TupleDescInitEntry(tupdesc,  9, "tables_completed", INT8OID,   -1, 0);
        TupleDescInitEntry(tupdesc, 10, "rows_copied",      INT8OID,   -1, 0);
        TupleDescInitEntry(tupdesc, 11, "bytes_copied",     INT8OID,   -1, 0);
        TupleDescInitEntry(tupdesc, 12, "elapsed_ms",       INT8OID,   -1, 0);
        TupleDescInitEntry(tupdesc, 13, "start_time",       TIMESTAMPTZOID, -1, 0);
        TupleDescInitEntry(tupdesc, 14, "end_time",         TIMESTAMPTZOID, -1, 0);
        TupleDescInitEntry(tupdesc, 15, "error_message",    TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc, 16, "pct_complete",     FLOAT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 17, "progress_bar",         TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc, 18, "elapsed_time",         TEXTOID,   -1, 0);

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        funcctx->user_fctx  = (void *)(intptr_t) 0;   /* slot_index */

        MemoryContextSwitchTo(oldctx);
    }

    funcctx    = SRF_PERCALL_SETUP();
    slot_index = (int)(intptr_t) funcctx->user_fctx;

    /* Scan shared memory slots for next non-free job */
    while (slot_index < PGCLONE_MAX_JOBS)
    {
        Datum       values[PGCLONE_VIEW_COLS];
        bool        nulls[PGCLONE_VIEW_COLS];
        HeapTuple   tuple;
        const char *status_str;
        const char *op_str;
        int64       elapsed_ms = 0;
        float8      pct = 0.0;

        LWLockAcquire(pgclone_state->lock, LW_SHARED);
        job = &pgclone_state->jobs[slot_index];

        if (job->status == PGCLONE_JOB_FREE)
        {
            LWLockRelease(pgclone_state->lock);
            slot_index++;
            continue;
        }

        /* Build the row while holding the lock */
        memset(nulls, 0, sizeof(nulls));

        /* status string */
        switch (job->status)
        {
            case PGCLONE_JOB_PENDING:   status_str = "pending";   break;
            case PGCLONE_JOB_RUNNING:   status_str = "running";   break;
            case PGCLONE_JOB_COMPLETED: status_str = "completed"; break;
            case PGCLONE_JOB_FAILED:    status_str = "failed";    break;
            case PGCLONE_JOB_CANCELLED: status_str = "cancelled"; break;
            default:                    status_str = "unknown";    break;
        }

        /* op_type string */
        switch (job->op_type)
        {
            case PGCLONE_OP_TABLE:    op_str = "table";    break;
            case PGCLONE_OP_SCHEMA:   op_str = "schema";   break;
            case PGCLONE_OP_DATABASE: op_str = "database"; break;
            default:                  op_str = "unknown";   break;
        }

        /* elapsed */
        if (job->start_time != 0)
        {
            if (job->end_time != 0)
                elapsed_ms = (job->end_time - job->start_time) / 1000;
            else
                elapsed_ms = (GetCurrentTimestamp() - job->start_time) / 1000;
        }

        /* percentage */
        if (job->total_tables > 0)
            pct = (float8) job->completed_tables / (float8) job->total_tables * 100.0;

        /* Fill datum array */
        values[0]  = Int32GetDatum(job->job_id);
        values[1]  = CStringGetTextDatum(status_str);
        values[2]  = CStringGetTextDatum(op_str);
        values[3]  = CStringGetTextDatum(job->schema_name);
        values[4]  = CStringGetTextDatum(job->table_name);
        values[5]  = CStringGetTextDatum(job->current_phase);
        values[6]  = CStringGetTextDatum(job->current_table);
        values[7]  = Int64GetDatum(job->total_tables);
        values[8]  = Int64GetDatum(job->completed_tables);
        values[9]  = Int64GetDatum(job->copied_rows);
        values[10] = Int64GetDatum(job->copied_bytes);
        values[11] = Int64GetDatum(elapsed_ms);

        if (job->start_time != 0)
            values[12] = TimestampTzGetDatum(job->start_time);
        else
            nulls[12] = true;

        if (job->end_time != 0)
            values[13] = TimestampTzGetDatum(job->end_time);
        else
            nulls[13] = true;

        if (job->error_message[0] != '\0')
            values[14] = CStringGetTextDatum(job->error_message);
        else
            nulls[14] = true;

        values[15] = Float8GetDatum(pct);

        /* Build progress bar with elapsed time */
        {
            char        bar[128];
            int         filled;
            int         empty;
            int         bi;
            int         pi;
            const int   bar_width = 20;
            long        elapsed_sec;
            char        elapsed_str[32];

            filled = (int)(pct / 100.0 * bar_width);
            if (filled > bar_width) filled = bar_width;
            empty = bar_width - filled;

            bi = 0;
            bar[bi++] = '[';
            for (pi = 0; pi < filled; pi++)
            {
                /* UTF-8 for █ (U+2588): 0xE2 0x96 0x88 */
                bar[bi++] = (char)0xE2;
                bar[bi++] = (char)0x96;
                bar[bi++] = (char)0x88;
            }
            for (pi = 0; pi < empty; pi++)
            {
                /* UTF-8 for ░ (U+2591): 0xE2 0x96 0x91 */
                bar[bi++] = (char)0xE2;
                bar[bi++] = (char)0x96;
                bar[bi++] = (char)0x91;
            }
            bar[bi++] = ']';
            bar[bi] = '\0';

            /* Format elapsed time as HH:MM:SS */
            elapsed_sec = elapsed_ms / 1000;
            snprintf(elapsed_str, sizeof(elapsed_str), "%02ld:%02ld:%02ld",
                     elapsed_sec / 3600,
                     (elapsed_sec % 3600) / 60,
                     elapsed_sec % 60);

            /* progress_bar column */
            {
                char full_bar[512];
                snprintf(full_bar, sizeof(full_bar),
                         "%s %.1f%% | %ld rows | %s elapsed",
                         bar, pct, (long) job->copied_rows,
                         elapsed_str);
                values[16] = CStringGetTextDatum(full_bar);
            }

            /* elapsed_time column (HH:MM:SS) */
            values[17] = CStringGetTextDatum(elapsed_str);
        }

        LWLockRelease(pgclone_state->lock);

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

        /* Advance to next slot for the next call */
        funcctx->user_fctx = (void *)(intptr_t)(slot_index + 1);

        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    SRF_RETURN_DONE(funcctx);
}
