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

PG_MODULE_MAGIC;

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
 * This is needed because SPI cannot handle COPY FROM STDIN.
 * We connect via the unix socket to the current database.
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
 * Uses COPY TO STDOUT on source and COPY FROM STDIN on target,
 * streaming data chunk by chunk without loading it all into memory.
 * This is dramatically faster than row-by-row INSERT.
 * --------------------------------------------------------------- */
static int64
pgx_clone_copy_data(PGconn *source_conn, PGconn *local_conn,
                    const char *schema_name, const char *source_table,
                    const char *target_table)
{
    PGresult       *res;
    StringInfoData  cmd;
    char           *buf;
    int             ret;
    int64           bytes_transferred = 0;
    int64           row_count = 0;

    /* ---- Start COPY OUT on source ---- */
    initStringInfo(&cmd);
    appendStringInfo(&cmd,
        "COPY %s.%s TO STDOUT WITH (FORMAT text)",
        quote_identifier(schema_name),
        quote_identifier(source_table));

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

    /* ---- Start COPY IN on local ---- */
    resetStringInfo(&cmd);
    appendStringInfo(&cmd,
        "COPY %s.%s FROM STDIN WITH (FORMAT text)",
        quote_identifier(schema_name),
        quote_identifier(target_table));

    res = PQexec(local_conn, cmd.data);
    pfree(cmd.data);

    if (PQresultStatus(res) != PGRES_COPY_IN)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
        PQclear(res);

        /* Cancel the source COPY */
        PQgetCopyData(source_conn, &buf, 0);
        if (buf) PQfreemem(buf);

        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgx_clone: COPY IN failed on local: %s", copy_errmsg)));
    }
    PQclear(res);

    /* ---- Stream data from source -> local ---- */
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

        /* Count rows (each line in text format = 1 row) */
        row_count++;

        /* Allow interrupt check periodically */
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

    /* End COPY on local (NULL = success) */
    if (PQputCopyEnd(local_conn, NULL) != 1)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgx_clone: error finalizing COPY on local: %s",
                        copy_errmsg)));
    }

    /* Get the final result from COPY */
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

    /* Extract actual row count from COPY result */
    row_count = atol(PQcmdTuples(res));
    PQclear(res);

    return row_count;
}

/* ---------------------------------------------------------------
 * Internal helper: clone indexes for a table from source.
 *
 * Fetches all index definitions using pg_get_indexdef() and
 * replaces the source table name with the target table name
 * if they differ. Skips indexes that back PRIMARY KEY or UNIQUE
 * constraints (those are created by pgx_clone_constraints).
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
 *
 * Handles: PRIMARY KEY, UNIQUE, CHECK, FOREIGN KEY constraints.
 * Uses pg_get_constraintdef() to get portable definitions.
 * Creates them in dependency order: PK -> UNIQUE -> CHECK -> FK.
 *
 * FK constraints that fail (e.g. referenced table not yet cloned)
 * are logged as WARNINGs and skipped — they can be retried later.
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
 *
 * Fetches trigger definitions using pg_get_triggerdef() and
 * replaces the source table name if target differs.
 * Skips internal triggers (created by constraints).
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
 * FUNCTION: pgx_clone_table(source_conninfo, schema, tablename, include_data, target_tablename)
 *
 * Clones a single table (structure + optionally data) from a remote
 * host to the local database. If target_tablename is provided, the
 * table will be created with that name instead of the source name.
 * Also clones indexes, constraints, and triggers.
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

    PGconn     *source_conn;
    PGconn     *local_conn;
    PGresult   *res;
    StringInfoData buf;
    int         idx_count, con_count, trig_count;

    /* Use target_tablename if provided (5th arg), otherwise use source name */
    if (PG_NARGS() >= 5 && !PG_ARGISNULL(4))
        target_name = text_to_cstring(PG_GETARG_TEXT_PP(4));
    else
        target_name = table_name;

    /* Connect to source */
    source_conn = pgx_clone_connect(source_conninfo);

    /* ---- Step 1: Get CREATE TABLE DDL from source ---- */
    initStringInfo(&buf);
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

    /*
     * Use loopback libpq connection for ALL local operations.
     * This avoids the SPI transaction visibility problem: SPI runs
     * inside the caller's transaction, so a separate connection
     * cannot see uncommitted SPI work. By using libpq for everything
     * (schema creation, table creation, data copy, indexes,
     * constraints, triggers), everything is in one consistent session.
     */
    local_conn = pgx_clone_connect_local();

    /* ---- Step 2: Create schema + table via loopback ---- */
    {
        PGresult *local_res;
        char     *ddl;

        ddl = pstrdup(PQgetvalue(res, 0, 0));
        PQclear(res);

        /* Create schema */
        resetStringInfo(&buf);
        appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                         quote_identifier(schema_name));
        pgx_clone_exec_conn(local_conn, buf.data);

        /* Create table */
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
                                        schema_name, table_name, target_name);

        ereport(NOTICE,
                (errmsg("pgx_clone: copied %ld rows into %s.%s using COPY protocol",
                        (long) row_count, schema_name, target_name)));
    }

    /* ---- Step 4: Clone constraints (PK -> UNIQUE -> CHECK -> FK) ---- */
    con_count = pgx_clone_constraints(source_conn, local_conn,
                                      schema_name, table_name, target_name);
    if (con_count > 0)
        ereport(NOTICE,
                (errmsg("pgx_clone: cloned %d constraints for %s.%s",
                        con_count, schema_name, target_name)));

    /* ---- Step 5: Clone indexes (non-constraint indexes only) ---- */
    idx_count = pgx_clone_indexes(source_conn, local_conn,
                                  schema_name, table_name, target_name);
    if (idx_count > 0)
        ereport(NOTICE,
                (errmsg("pgx_clone: cloned %d indexes for %s.%s",
                        idx_count, schema_name, target_name)));

    /* ---- Step 6: Clone triggers ---- */
    trig_count = pgx_clone_triggers(source_conn, local_conn,
                                    schema_name, table_name, target_name);
    if (trig_count > 0)
        ereport(NOTICE,
                (errmsg("pgx_clone: cloned %d triggers for %s.%s",
                        trig_count, schema_name, target_name)));

    PQfinish(local_conn);
    PQfinish(source_conn);

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgx_clone_schema(source_conninfo, schema, include_data)
 *
 * Clones an entire schema: tables, sequences, views, functions.
 * Indexes, constraints, and triggers are handled per-table by
 * pgx_clone_table.
 *
 * FK constraints between tables in the same schema may fail during
 * table cloning if the referenced table hasn't been created yet.
 * After all tables are cloned, we do a second pass for any
 * deferred FK constraints.
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_schema);

Datum
pgx_clone_schema(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    bool        include_data      = PG_GETARG_BOOL(2);

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);

    PGconn     *source_conn;
    PGconn     *local_conn;
    PGresult   *res;
    StringInfoData buf;
    int         i, ntables;
    char      **table_names = NULL;

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

    PQfinish(local_conn);  /* close local conn, pgx_clone_table opens its own */

    /* ---- Step 4: Clone tables (each handles its own
     *              indexes, constraints, and triggers) ---- */
    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT tablename FROM pg_catalog.pg_tables "
        "WHERE schemaname = %s ORDER BY tablename",
        quote_literal_cstr(schema_name));

    res = pgx_clone_exec(source_conn, buf.data);

    ntables = PQntuples(res);

    /* Save table names before PQclear */
    if (ntables > 0)
    {
        table_names = palloc(sizeof(char *) * ntables);
        for (i = 0; i < ntables; i++)
            table_names[i] = pstrdup(PQgetvalue(res, i, 0));
    }
    PQclear(res);

    PQfinish(source_conn);  /* close source, pgx_clone_table opens its own */

    for (i = 0; i < ntables; i++)
    {
        Datum result;

        result = DirectFunctionCall4(pgx_clone_table,
                    CStringGetTextDatum(source_conninfo),
                    CStringGetTextDatum(schema_name),
                    CStringGetTextDatum(table_names[i]),
                    BoolGetDatum(include_data));
        (void) result;
    }

    ereport(NOTICE,
            (errmsg("pgx_clone: cloned %d tables (with indexes, constraints, triggers) from schema %s",
                    ntables, schema_name)));

    /* ---- Step 5: Retry failed FK constraints ----
     * Some FK constraints may have failed if the referenced table
     * was cloned after the referencing table. Now all tables exist,
     * so we retry any missing FK constraints.
     */
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

                    /* Try to add, ignore if already exists */
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

    /* Cleanup table_names */
    if (table_names)
    {
        for (i = 0; i < ntables; i++)
            pfree(table_names[i]);
        pfree(table_names);
    }

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgx_clone_functions(source_conninfo, schema)
 *
 * Clones all functions/procedures from a schema.
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

    /* Ensure schema exists */
    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                     quote_identifier(schema_name));
    pgx_clone_exec_conn(local_conn, buf.data);

    /* Get all function definitions */
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
 * FUNCTION: pgx_clone_database(source_conninfo, include_data)
 *
 * Clones all user schemas from the source database.
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_database);

Datum
pgx_clone_database(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    bool        include_data      = PG_GETARG_BOOL(1);

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);

    PGconn     *source_conn;
    PGresult   *res;
    int         i, nschemas;
    char      **schema_names;

    source_conn = pgx_clone_connect(source_conninfo);

    /* Get all user schemas (exclude system schemas) */
    res = pgx_clone_exec(source_conn,
        "SELECT nspname FROM pg_catalog.pg_namespace "
        "WHERE nspname NOT LIKE 'pg_%' "
        "AND nspname <> 'information_schema' "
        "ORDER BY nspname");

    nschemas = PQntuples(res);

    /* Save schema names before closing connection */
    schema_names = palloc(sizeof(char *) * nschemas);
    for (i = 0; i < nschemas; i++)
        schema_names[i] = pstrdup(PQgetvalue(res, i, 0));

    PQclear(res);
    PQfinish(source_conn);

    for (i = 0; i < nschemas; i++)
    {
        Datum result;

        ereport(NOTICE,
                (errmsg("pgx_clone: cloning schema %s (%d/%d)",
                        schema_names[i], i + 1, nschemas)));

        result = DirectFunctionCall3(pgx_clone_schema,
                    CStringGetTextDatum(source_conninfo),
                    CStringGetTextDatum(schema_names[i]),
                    BoolGetDatum(include_data));
        (void) result;
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
 *
 * Returns the extension version string.
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgx_clone_version);

Datum
pgx_clone_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("pgx_clone 0.2.0"));
}
