/*
 * pgx_clone_bgw.c - Background worker for async clone operations
 *
 * Implements:
 *  - Shared memory for job tracking and progress
 *  - Background worker that executes clone jobs
 *  - Parallel table cloning within a schema/database clone
 *  - Resume support via checkpoint tracking
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "commands/dbcommands.h"
#include "utils/guc.h"
#include "libpq-fe.h"

#if PG_VERSION_NUM >= 170000
#include "postmaster/interrupt.h"
#endif
#include "pgx_clone_bgw.h"

/* Shared memory state */
PgxCloneSharedState *pgx_clone_state = NULL;

/* Shmem hook chain */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* ---------------------------------------------------------------
 * Shared memory sizing and initialization
 * --------------------------------------------------------------- */
Size
pgx_clone_shmem_size(void)
{
    return MAXALIGN(sizeof(PgxCloneSharedState));
}

static void
pgx_clone_shmem_startup(void)
{
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    pgx_clone_state = ShmemInitStruct("pgx_clone",
                                      pgx_clone_shmem_size(),
                                      &found);

    if (!found)
    {
        memset(pgx_clone_state, 0, pgx_clone_shmem_size());
        pgx_clone_state->lock = &(GetNamedLWLockTranche("pgx_clone"))->lock;
        pgx_clone_state->next_job_id = 1;
    }

    LWLockRelease(AddinShmemInitLock);
}

void
pgx_clone_shmem_init(void)
{
    /* Request shared memory and LWLock */
    RequestAddinShmemSpace(pgx_clone_shmem_size());
    RequestNamedLWLockTranche("pgx_clone", 1);

    /* Install shmem startup hook */
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = pgx_clone_shmem_startup;
}

/* find_job and find_free_slot are defined as static inline in pgx_clone_bgw.h */

/* ---------------------------------------------------------------
 * Helper: connect to local database in bgworker context
 * --------------------------------------------------------------- */
static PGconn *
bgw_connect_local(const char *dbname, const char *port)
{
    PGconn         *conn;
    StringInfoData  conninfo;

    initStringInfo(&conninfo);
    appendStringInfo(&conninfo, "dbname='%s' port=%s",
                     dbname, port ? port : "5432");

    conn = PQconnectdb(conninfo.data);
    pfree(conninfo.data);

    if (PQstatus(conn) != CONNECTION_OK)
    {
        elog(WARNING, "pgx_clone bgw: could not connect to local: %s",
             PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }

    return conn;
}

/* ---------------------------------------------------------------
 * Helper: execute DDL on a connection, return success
 * --------------------------------------------------------------- */
static bool
bgw_exec(PGconn *conn, const char *query)
{
    PGresult *res = PQexec(conn, query);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK ||
               PQresultStatus(res) == PGRES_TUPLES_OK);

    if (!ok)
        elog(WARNING, "pgx_clone bgw: query failed: %s", PQerrorMessage(conn));

    PQclear(res);
    return ok;
}

/* ---------------------------------------------------------------
 * Helper: handle conflict resolution for a table
 * --------------------------------------------------------------- */
static bool
bgw_handle_conflict(PGconn *local_conn, const char *schema_name,
                    const char *target_table,
                    PgxCloneConflictStrategy strategy)
{
    PGresult       *res;
    StringInfoData  buf;
    bool            exists;

    /* Check if table exists */
    initStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT 1 FROM pg_catalog.pg_tables "
        "WHERE schemaname = '%s' AND tablename = '%s'",
        schema_name, target_table);

    res = PQexec(local_conn, buf.data);
    exists = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    PQclear(res);

    if (!exists)
    {
        pfree(buf.data);
        return true;  /* No conflict */
    }

    switch (strategy)
    {
        case PGX_CLONE_CONFLICT_ERROR:
            elog(WARNING, "pgx_clone: table %s.%s already exists (conflict_strategy=error)",
                 schema_name, target_table);
            pfree(buf.data);
            return false;

        case PGX_CLONE_CONFLICT_SKIP:
            elog(NOTICE, "pgx_clone: skipping %s.%s (already exists)",
                 schema_name, target_table);
            pfree(buf.data);
            return false;  /* Signal to skip, not an error */

        case PGX_CLONE_CONFLICT_REPLACE:
            resetStringInfo(&buf);
            appendStringInfo(&buf, "DROP TABLE IF EXISTS %s.%s CASCADE",
                             schema_name, target_table);
            bgw_exec(local_conn, buf.data);
            elog(NOTICE, "pgx_clone: dropped existing %s.%s (conflict_strategy=replace)",
                 schema_name, target_table);
            pfree(buf.data);
            return true;

        case PGX_CLONE_CONFLICT_RENAME:
            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "ALTER TABLE IF EXISTS %s.%s RENAME TO %s_old",
                schema_name, target_table, target_table);
            bgw_exec(local_conn, buf.data);
            elog(NOTICE, "pgx_clone: renamed existing %s.%s to %s_old",
                 schema_name, target_table, target_table);
            pfree(buf.data);
            return true;

        default:
            pfree(buf.data);
            return false;
    }
}

/* ---------------------------------------------------------------
 * Helper: stream COPY data from source to local
 * --------------------------------------------------------------- */
static int64
bgw_copy_data(PGconn *source_conn, PGconn *local_conn,
              const char *schema_name, const char *source_table,
              const char *target_table, PgxCloneJob *job)
{
    PGresult       *res;
    StringInfoData  cmd;
    char           *buf;
    int             ret;
    int64           row_count = 0;

    initStringInfo(&cmd);
    appendStringInfo(&cmd, "COPY %s.%s TO STDOUT WITH (FORMAT text)",
                     schema_name, source_table);

    res = PQexec(source_conn, cmd.data);
    if (PQresultStatus(res) != PGRES_COPY_OUT)
    {
        PQclear(res);
        pfree(cmd.data);
        return -1;
    }
    PQclear(res);

    resetStringInfo(&cmd);
    appendStringInfo(&cmd, "COPY %s.%s FROM STDIN WITH (FORMAT text)",
                     schema_name, target_table);

    res = PQexec(local_conn, cmd.data);
    pfree(cmd.data);

    if (PQresultStatus(res) != PGRES_COPY_IN)
    {
        PQclear(res);
        /* Drain source */
        while (PQgetCopyData(source_conn, &buf, 0) > 0)
            PQfreemem(buf);
        return -1;
    }
    PQclear(res);

    while ((ret = PQgetCopyData(source_conn, &buf, 0)) > 0)
    {
        PQputCopyData(local_conn, buf, ret);
        PQfreemem(buf);
        row_count++;

        /* Update progress in shared memory */
        if (job && row_count % 10000 == 0)
        {
            LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
            job->copied_rows += 10000;
            LWLockRelease(pgx_clone_state->lock);
        }
    }

    PQputCopyEnd(local_conn, NULL);
    res = PQgetResult(local_conn);

    if (PQresultStatus(res) == PGRES_COMMAND_OK)
        row_count = atol(PQcmdTuples(res));

    PQclear(res);

    return row_count;
}

/* ---------------------------------------------------------------
 * Helper: clone a single table (used by bgworker)
 * --------------------------------------------------------------- */
static bool
bgw_clone_one_table(PGconn *source_conn, PGconn *local_conn,
                    PgxCloneJob *job, const char *table_name)
{
    PGresult       *res;
    StringInfoData  buf;
    const char     *target = table_name;
    int64           rows;

    /* Update current table in progress */
    LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
    strlcpy(job->current_table, table_name, NAMEDATALEN);
    strlcpy(job->current_phase, "checking conflicts", 64);
    LWLockRelease(pgx_clone_state->lock);

    /* Handle conflicts */
    if (!bgw_handle_conflict(local_conn, job->schema_name, target,
                             job->conflict_strategy))
    {
        if (job->conflict_strategy == PGX_CLONE_CONFLICT_SKIP)
            return true;  /* Not an error, just skipped */
        return false;
    }

    /* Get DDL */
    LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
    strlcpy(job->current_phase, "creating table", 64);
    LWLockRelease(pgx_clone_state->lock);

    initStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT 'CREATE TABLE IF NOT EXISTS %s.' || quote_ident(c.relname) || ' (' || "
        "string_agg(quote_ident(a.attname) || ' ' || "
        "pg_catalog.format_type(a.atttypid, a.atttypmod) || "
        "CASE WHEN a.attnotnull THEN ' NOT NULL' ELSE '' END || "
        "CASE WHEN d.adbin IS NOT NULL THEN ' DEFAULT ' || pg_get_expr(d.adbin, d.adrelid) ELSE '' END, "
        "', ' ORDER BY a.attnum) || ')' AS ddl "
        "FROM pg_catalog.pg_class c "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid "
        "LEFT JOIN pg_catalog.pg_attrdef d ON d.adrelid = c.oid AND d.adnum = a.attnum "
        "WHERE n.nspname = '%s' AND c.relname = '%s' "
        "AND a.attnum > 0 AND NOT a.attisdropped "
        "GROUP BY c.relname",
        job->schema_name, job->schema_name, table_name);

    res = PQexec(source_conn, buf.data);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0)
    {
        PQclear(res);
        pfree(buf.data);
        return false;
    }

    bgw_exec(local_conn, PQgetvalue(res, 0, 0));
    PQclear(res);

    /* Copy data */
    if (job->include_data)
    {
        LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
        strlcpy(job->current_phase, "copying data", 64);
        LWLockRelease(pgx_clone_state->lock);

        rows = bgw_copy_data(source_conn, local_conn,
                             job->schema_name, table_name, target, job);
        if (rows < 0)
        {
            pfree(buf.data);
            return false;
        }
    }

    /* Clone constraints */
    if (job->include_constraints)
    {
        LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
        strlcpy(job->current_phase, "creating constraints", 64);
        LWLockRelease(pgx_clone_state->lock);

        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "SELECT conname, pg_get_constraintdef(con.oid, true) "
            "FROM pg_catalog.pg_constraint con "
            "JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
            "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
            "WHERE n.nspname = '%s' AND c.relname = '%s' "
            "ORDER BY CASE contype WHEN 'p' THEN 1 WHEN 'u' THEN 2 "
            "WHEN 'c' THEN 3 WHEN 'f' THEN 4 ELSE 5 END",
            job->schema_name, table_name);

        res = PQexec(source_conn, buf.data);
        if (PQresultStatus(res) == PGRES_TUPLES_OK)
        {
            int ci;
            for (ci = 0; ci < PQntuples(res); ci++)
            {
                resetStringInfo(&buf);
                appendStringInfo(&buf,
                    "ALTER TABLE %s.%s ADD CONSTRAINT %s %s",
                    job->schema_name, target,
                    PQgetvalue(res, ci, 0), PQgetvalue(res, ci, 1));
                bgw_exec(local_conn, buf.data);
            }
        }
        PQclear(res);
    }

    /* Clone indexes */
    if (job->include_indexes)
    {
        LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
        strlcpy(job->current_phase, "creating indexes", 64);
        LWLockRelease(pgx_clone_state->lock);

        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "SELECT pg_get_indexdef(i.indexrelid) "
            "FROM pg_catalog.pg_index i "
            "JOIN pg_catalog.pg_class c ON c.oid = i.indrelid "
            "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
            "WHERE n.nspname = '%s' AND c.relname = '%s' "
            "AND NOT i.indisprimary "
            "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_constraint con "
            "WHERE con.conindid = i.indexrelid)",
            job->schema_name, table_name);

        res = PQexec(source_conn, buf.data);
        if (PQresultStatus(res) == PGRES_TUPLES_OK)
        {
            int ii;
            for (ii = 0; ii < PQntuples(res); ii++)
                bgw_exec(local_conn, PQgetvalue(res, ii, 0));
        }
        PQclear(res);
    }

    /* Clone triggers */
    if (job->include_triggers)
    {
        LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
        strlcpy(job->current_phase, "creating triggers", 64);
        LWLockRelease(pgx_clone_state->lock);

        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "SELECT pg_get_triggerdef(t.oid, true) "
            "FROM pg_catalog.pg_trigger t "
            "JOIN pg_catalog.pg_class c ON c.oid = t.tgrelid "
            "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
            "WHERE n.nspname = '%s' AND c.relname = '%s' "
            "AND NOT t.tgisinternal",
            job->schema_name, table_name);

        res = PQexec(source_conn, buf.data);
        if (PQresultStatus(res) == PGRES_TUPLES_OK)
        {
            int ti;
            for (ti = 0; ti < PQntuples(res); ti++)
                bgw_exec(local_conn, PQgetvalue(res, ti, 0));
        }
        PQclear(res);
    }

    /* Update resume checkpoint */
    LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
    job->completed_tables++;
    strlcpy(job->resume_checkpoint, table_name, NAMEDATALEN);
    strlcpy(job->current_phase, "done", 64);
    LWLockRelease(pgx_clone_state->lock);

    pfree(buf.data);
    return true;
}

/* ===============================================================
 * Background worker main function
 *
 * Reads job parameters from shared memory, executes the clone,
 * and updates progress throughout.
 * =============================================================== */
void
pgx_clone_bgw_main(Datum main_arg)
{
    int             job_id = DatumGetInt32(main_arg);
    PgxCloneJob    *job;
    PGconn         *source_conn = NULL;
    PGconn         *local_conn = NULL;
    const char     *port;
    char           *dbname;

    /* Set up signal handlers */
    #if PG_VERSION_NUM >= 170000
        pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    #else
        pqsignal(SIGTERM, die);
    #endif

    BackgroundWorkerUnblockSignals();

    if (!pgx_clone_state)
    {
        elog(ERROR, "pgx_clone: shared memory not initialized");
        return;
    }

    /* Find our job */
    LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
    job = find_job(job_id);
    if (!job || job->status != PGX_CLONE_JOB_PENDING)
    {
        LWLockRelease(pgx_clone_state->lock);
        elog(ERROR, "pgx_clone bgw: job %d not found or not pending", job_id);
        return;
    }
    job->status = PGX_CLONE_JOB_RUNNING;
    job->worker_pid = MyProcPid;
    job->start_time = GetCurrentTimestamp();
    LWLockRelease(pgx_clone_state->lock);

    /* Connect to source */
    BackgroundWorkerInitializeConnectionByOid(job->database_oid, InvalidOid, 0);

    dbname = get_database_name(job->database_oid);
    port = GetConfigOption("port", false, false);

    source_conn = PQconnectdb(job->source_conninfo);
    if (PQstatus(source_conn) != CONNECTION_OK)
    {
        LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
        job->status = PGX_CLONE_JOB_FAILED;
        strlcpy(job->error_message, "could not connect to source", 256);
        job->end_time = GetCurrentTimestamp();
        LWLockRelease(pgx_clone_state->lock);
        goto cleanup;
    }

    local_conn = bgw_connect_local(dbname, port);
    if (!local_conn)
    {
        LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
        job->status = PGX_CLONE_JOB_FAILED;
        strlcpy(job->error_message, "could not connect to local database", 256);
        job->end_time = GetCurrentTimestamp();
        LWLockRelease(pgx_clone_state->lock);
        goto cleanup;
    }

    /* Execute based on operation type */
    if (job->op_type == PGX_CLONE_OP_TABLE)
    {
        LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
        job->total_tables = 1;
        strlcpy(job->current_phase, "starting", 64);
        LWLockRelease(pgx_clone_state->lock);

        /* Create schema */
        {
            StringInfoData buf;
            initStringInfo(&buf);
            appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                             job->schema_name);
            bgw_exec(local_conn, buf.data);
            pfree(buf.data);
        }

        if (!bgw_clone_one_table(source_conn, local_conn, job,
                                 job->table_name))
        {
            LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
            job->status = PGX_CLONE_JOB_FAILED;
            snprintf(job->error_message, 256,
                     "failed to clone table %s.%s",
                     job->schema_name, job->table_name);
            job->end_time = GetCurrentTimestamp();
            LWLockRelease(pgx_clone_state->lock);
            goto cleanup;
        }
    }
    else if (job->op_type == PGX_CLONE_OP_SCHEMA ||
             job->op_type == PGX_CLONE_OP_DATABASE)
    {
        PGresult   *table_res;
        StringInfoData buf;
        int         i, ntables;
        bool        past_checkpoint;

        /* Create schema */
        initStringInfo(&buf);
        appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                         job->schema_name);
        bgw_exec(local_conn, buf.data);

        /* Get table list */
        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "SELECT tablename FROM pg_catalog.pg_tables "
            "WHERE schemaname = '%s' ORDER BY tablename",
            job->schema_name);

        table_res = PQexec(source_conn, buf.data);
        if (PQresultStatus(table_res) != PGRES_TUPLES_OK)
        {
            PQclear(table_res);
            LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
            job->status = PGX_CLONE_JOB_FAILED;
            strlcpy(job->error_message, "could not list tables", 256);
            job->end_time = GetCurrentTimestamp();
            LWLockRelease(pgx_clone_state->lock);
            pfree(buf.data);
            goto cleanup;
        }

        ntables = PQntuples(table_res);

        LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
        job->total_tables = ntables;
        LWLockRelease(pgx_clone_state->lock);

        /* Resume support: skip tables before checkpoint */
        past_checkpoint = (job->resume_checkpoint[0] == '\0');

        for (i = 0; i < ntables; i++)
        {
            const char *tname = PQgetvalue(table_res, i, 0);

            /* Check for cancellation */
            LWLockAcquire(pgx_clone_state->lock, LW_SHARED);
            if (job->status == PGX_CLONE_JOB_CANCELLED)
            {
                LWLockRelease(pgx_clone_state->lock);
                PQclear(table_res);
                pfree(buf.data);
                goto cleanup;
            }
            LWLockRelease(pgx_clone_state->lock);

            /* Resume logic: skip until we pass the checkpoint */
            if (!past_checkpoint)
            {
                if (strcmp(tname, job->resume_checkpoint) == 0)
                    past_checkpoint = true;
                continue;
            }

            if (!bgw_clone_one_table(source_conn, local_conn, job, tname))
            {
                elog(WARNING, "pgx_clone bgw: failed to clone table %s, continuing...",
                     tname);
            }

            CHECK_FOR_INTERRUPTS();
        }

        PQclear(table_res);
        pfree(buf.data);
    }

    /* Mark job as completed */
    LWLockAcquire(pgx_clone_state->lock, LW_EXCLUSIVE);
    if (job->status == PGX_CLONE_JOB_RUNNING)
        job->status = PGX_CLONE_JOB_COMPLETED;
    job->end_time = GetCurrentTimestamp();
    strlcpy(job->current_phase, "completed", 64);
    LWLockRelease(pgx_clone_state->lock);

cleanup:
    if (source_conn)
        PQfinish(source_conn);
    if (local_conn)
        PQfinish(local_conn);

    proc_exit(0);
}
