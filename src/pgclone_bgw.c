/*
 * pgclone_bgw.c - Background worker for async clone operations
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
#else
#include "tcop/tcopprot.h"
#endif
#include "pgclone_bgw.h"

/* Shared memory state */
PgcloneSharedState *pgclone_state = NULL;

/* Shmem hook chain */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;
#endif

/* ---------------------------------------------------------------
 * Shared memory sizing and initialization
 * --------------------------------------------------------------- */
Size
pgclone_shmem_size(void)
{
    return MAXALIGN(sizeof(PgcloneSharedState));
}

static void
pgclone_shmem_startup(void)
{
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    pgclone_state = ShmemInitStruct("pgclone",
                                      pgclone_shmem_size(),
                                      &found);

    if (!found)
    {
        memset(pgclone_state, 0, pgclone_shmem_size());
        pgclone_state->lock = &(GetNamedLWLockTranche("pgclone"))->lock;
        pgclone_state->next_job_id = 1;
    }

    LWLockRelease(AddinShmemInitLock);
}

/*
 * Request shared memory space and LWLocks.
 *
 * PG 15+ requires this to happen inside shmem_request_hook.
 * PG 14 and earlier allow it directly in _PG_init.
 */
static void
pgclone_shmem_request(void)
{
#if PG_VERSION_NUM >= 150000
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();
#endif

    RequestAddinShmemSpace(pgclone_shmem_size());
    RequestNamedLWLockTranche("pgclone", 1);
}

void
pgclone_shmem_init(void)
{
#if PG_VERSION_NUM >= 150000
    /* PG 15+: register via shmem_request_hook */
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = pgclone_shmem_request;
#else
    /* PG 14 and earlier: request directly */
    pgclone_shmem_request();
#endif

    /* Install shmem startup hook (works the same on all versions) */
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = pgclone_shmem_startup;
}

/* find_job and find_free_slot are defined as static inline in pgclone_bgw.h */

/* ---------------------------------------------------------------
 * Helper: connect to local database in bgworker context.
 * Prefers Unix domain socket over TCP 127.0.0.1.
 * --------------------------------------------------------------- */
static PGconn *
bgw_connect_local(const char *dbname, const char *port, const char *username)
{
    PGconn         *conn;
    StringInfoData  conninfo;
    const char     *socket_dir;

    socket_dir = GetConfigOption("unix_socket_directories", false, false);

    initStringInfo(&conninfo);

    if (socket_dir && socket_dir[0])
    {
        char *first_dir = pstrdup(socket_dir);
        char *comma = strchr(first_dir, ',');
        if (comma)
            *comma = '\0';

        /* Trim trailing whitespace */
        {
            int len = strlen(first_dir);
            while (len > 0 && first_dir[len - 1] == ' ')
                first_dir[--len] = '\0';
        }

        appendStringInfo(&conninfo, "host=%s dbname='%s' port=%s user=%s",
                         first_dir,
                         dbname, port ? port : "5432",
                         username && username[0] ? username : "postgres");
        pfree(first_dir);
    }
    else
    {
        appendStringInfo(&conninfo, "host=127.0.0.1 dbname='%s' port=%s user=%s",
                         dbname, port ? port : "5432",
                         username && username[0] ? username : "postgres");
    }

    conn = PQconnectdb(conninfo.data);
    pfree(conninfo.data);

    if (PQstatus(conn) != CONNECTION_OK)
    {
        elog(WARNING, "pgclone bgw: could not connect to local: %s",
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
        elog(WARNING, "pgclone bgw: query failed: %s", PQerrorMessage(conn));

    PQclear(res);
    return ok;
}

/* ---------------------------------------------------------------
 * Helper: handle conflict resolution for a table
 * --------------------------------------------------------------- */
static bool
bgw_handle_conflict(PGconn *local_conn, const char *schema_name,
                    const char *target_table,
                    PgcloneConflictStrategy strategy)
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
        case PGCLONE_CONFLICT_ERROR:
            elog(WARNING, "pgclone: table %s.%s already exists (conflict_strategy=error)",
                 schema_name, target_table);
            pfree(buf.data);
            return false;

        case PGCLONE_CONFLICT_SKIP:
            elog(NOTICE, "pgclone: skipping %s.%s (already exists)",
                 schema_name, target_table);
            pfree(buf.data);
            return false;  /* Signal to skip, not an error */

        case PGCLONE_CONFLICT_REPLACE:
            resetStringInfo(&buf);
            appendStringInfo(&buf, "DROP TABLE IF EXISTS %s.%s CASCADE",
                             schema_name, target_table);
            bgw_exec(local_conn, buf.data);
            elog(NOTICE, "pgclone: dropped existing %s.%s (conflict_strategy=replace)",
                 schema_name, target_table);
            pfree(buf.data);
            return true;

        case PGCLONE_CONFLICT_RENAME:
            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "ALTER TABLE IF EXISTS %s.%s RENAME TO %s_old",
                schema_name, target_table, target_table);
            bgw_exec(local_conn, buf.data);
            elog(NOTICE, "pgclone: renamed existing %s.%s to %s_old",
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
              const char *target_table, PgcloneJob *job)
{
    PGresult       *res;
    StringInfoData  cmd;
    char           *buf;
    int             ret;
    int64           chunk_count = 0;
    int64           row_count = 0;

    initStringInfo(&cmd);
    appendStringInfo(&cmd, "COPY %s.%s TO STDOUT WITH (FORMAT text)",
                     schema_name, source_table);

    res = PQexec(source_conn, cmd.data);
    if (PQresultStatus(res) != PGRES_COPY_OUT)
    {
        elog(WARNING, "pgclone bgw: COPY TO STDOUT failed: %s",
             PQerrorMessage(source_conn));
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
        elog(WARNING, "pgclone bgw: COPY FROM STDIN failed: %s",
             PQerrorMessage(local_conn));
        PQclear(res);
        /* Drain source */
        while (PQgetCopyData(source_conn, &buf, 0) > 0)
            PQfreemem(buf);
        /* Consume source COPY result */
        res = PQgetResult(source_conn);
        if (res) PQclear(res);
        return -1;
    }
    PQclear(res);

    while ((ret = PQgetCopyData(source_conn, &buf, 0)) > 0)
    {
        PQputCopyData(local_conn, buf, ret);
        PQfreemem(buf);
        chunk_count++;

        /* Update progress in shared memory every 10000 rows */
        if (job && chunk_count % 10000 == 0)
        {
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            job->copied_rows = chunk_count;
            LWLockRelease(pgclone_state->lock);
        }
    }

    /* Consume the source COPY OUT completion result */
    res = PQgetResult(source_conn);
    if (res) PQclear(res);

    PQputCopyEnd(local_conn, NULL);
    res = PQgetResult(local_conn);

    if (PQresultStatus(res) == PGRES_COMMAND_OK)
        row_count = atol(PQcmdTuples(res));
    else
    {
        elog(WARNING, "pgclone bgw: COPY completed with error: %s",
             PQerrorMessage(local_conn));
        PQclear(res);
        return -1;
    }

    PQclear(res);

    /* Final row count update */
    if (job)
    {
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        job->copied_rows = row_count;
        LWLockRelease(pgclone_state->lock);
    }

    return row_count;
}

/* ---------------------------------------------------------------
 * Helper: clone a single table (used by bgworker)
 * --------------------------------------------------------------- */
static bool
bgw_clone_one_table(PGconn *source_conn, PGconn *local_conn,
                    PgcloneJob *job, const char *table_name)
{
    PGresult       *res;
    StringInfoData  buf;
    const char     *target;
    int64           rows;

    /* Use target_name if set and different from source table */
    if (job->target_name[0] != '\0' && strcmp(job->target_name, table_name) != 0)
        target = job->target_name;
    else
        target = table_name;


    /* Update current table in progress */
    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
    strlcpy(job->current_table, table_name, NAMEDATALEN);
    strlcpy(job->current_phase, "checking conflicts", 64);
    LWLockRelease(pgclone_state->lock);

    /* Handle conflicts */
    if (!bgw_handle_conflict(local_conn, job->schema_name, target,
                             job->conflict_strategy))
    {
        if (job->conflict_strategy == PGCLONE_CONFLICT_SKIP)
            return true;  /* Not an error, just skipped */
        return false;
    }

    /* Get DDL */
    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
    strlcpy(job->current_phase, "creating table", 64);
    LWLockRelease(pgclone_state->lock);

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
        "WHERE n.nspname = '%s' AND c.relname = '%s' "
        "AND a.attnum > 0 AND NOT a.attisdropped "
        "GROUP BY c.relname",
        job->schema_name, target, job->schema_name, table_name);

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
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        strlcpy(job->current_phase, "copying data", 64);
        LWLockRelease(pgclone_state->lock);

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
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        strlcpy(job->current_phase, "creating constraints", 64);
        LWLockRelease(pgclone_state->lock);

        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "SELECT conname, pg_get_constraintdef(con.oid, true) "
            "FROM pg_catalog.pg_constraint con "
            "JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
            "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
            "WHERE n.nspname = '%s' AND c.relname = '%s' "
            "AND con.contype != 'n' "
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
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        strlcpy(job->current_phase, "creating indexes", 64);
        LWLockRelease(pgclone_state->lock);

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
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        strlcpy(job->current_phase, "creating triggers", 64);
        LWLockRelease(pgclone_state->lock);

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
    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
    job->completed_tables++;
    strlcpy(job->resume_checkpoint, table_name, NAMEDATALEN);
    strlcpy(job->current_phase, "done", 64);
    LWLockRelease(pgclone_state->lock);

    pfree(buf.data);
    return true;
}

/* ===============================================================
 * Background worker main function
 *
 * Reads job parameters from shared memory, executes the clone,
 * and updates progress throughout.
 *
 * Note: We use __attribute__((visibility("default"))) directly
 * because PGDLLEXPORT may be a no-op on some PostgreSQL versions
 * when compiled with -fvisibility=hidden. The bgworker entry point
 * MUST be visible to PostgreSQL's dynamic linker.
 * =============================================================== */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
void
pgclone_bgw_main(Datum main_arg)
{
    int             job_id = DatumGetInt32(main_arg);
    PgcloneJob    *job;
    PGconn         *source_conn = NULL;
    PGconn         *local_conn = NULL;
    const char     *port;
    const char     *dbname;

    /* Very first thing — log that we entered the function */

    /* Set up signal handlers */
    #if PG_VERSION_NUM >= 170000
        pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    #else
        pqsignal(SIGTERM, die);
    #endif

    BackgroundWorkerUnblockSignals();


    if (!pgclone_state)
    {
        elog(ERROR, "pgclone: shared memory not initialized");
        return;
    }


    /* Find our job and mark as running */
    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

    job = find_job(job_id);
    if (!job || job->status != PGCLONE_JOB_PENDING)
    {
        LWLockRelease(pgclone_state->lock);
        elog(ERROR, "pgclone bgw: job %d not found or not pending (job=%p status=%d)",
             job_id, (void*)job, job ? job->status : -1);
        return;
    }
    job->status = PGCLONE_JOB_RUNNING;
    job->worker_pid = MyProcPid;
    job->start_time = GetCurrentTimestamp();
    LWLockRelease(pgclone_state->lock);

    /*
     * Initialize database connection. This MUST happen before any
     * palloc, SPI, or catalog access. Note: shared memory pointers
     * remain valid after this call since we use ShmemInitStruct.
     */
    BackgroundWorkerInitializeConnectionByOid(job->database_oid, InvalidOid, 0);

    /* Use database name stored in job (get_database_name needs catalog access
     * which may not be available in bgworker without an active transaction) */
    dbname = job->database_name;
    port = GetConfigOption("port", false, false);


    source_conn = PQconnectdb(job->source_conninfo);
    if (PQstatus(source_conn) != CONNECTION_OK)
    {
        elog(WARNING, "pgclone bgw: source connection failed: %s",
             PQerrorMessage(source_conn));
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        job->status = PGCLONE_JOB_FAILED;
        strlcpy(job->error_message, "could not connect to source", 256);
        job->end_time = GetCurrentTimestamp();
        LWLockRelease(pgclone_state->lock);
        goto cleanup;
    }

    /* Pin source search_path to pg_catalog so pg_get_triggerdef(),
     * pg_get_expr() and friends emit fully schema-qualified relation
     * names regardless of the source DB's default search_path.
     * See pgclone_normalize_session() in pgclone.c for the rationale. */
    {
        PGresult *sp_res = PQexec(source_conn, "SET search_path = pg_catalog");
        if (PQresultStatus(sp_res) != PGRES_COMMAND_OK)
            elog(WARNING, "pgclone bgw: could not set source search_path: %s",
                 PQerrorMessage(source_conn));
        PQclear(sp_res);
    }

    local_conn = bgw_connect_local(dbname, port, job->username);
    if (!local_conn)
    {
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        job->status = PGCLONE_JOB_FAILED;
        strlcpy(job->error_message, "could not connect to local database", 256);
        job->end_time = GetCurrentTimestamp();
        LWLockRelease(pgclone_state->lock);
        goto cleanup;
    }

    /* Execute based on operation type */
    if (job->op_type == PGCLONE_OP_TABLE)
    {
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        job->total_tables = 1;
        strlcpy(job->current_phase, "starting", 64);
        LWLockRelease(pgclone_state->lock);

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
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            job->status = PGCLONE_JOB_FAILED;
            snprintf(job->error_message, 256,
                     "failed to clone table %s.%s",
                     job->schema_name, job->table_name);
            job->end_time = GetCurrentTimestamp();
            LWLockRelease(pgclone_state->lock);
            goto cleanup;
        }
    }
    else if (job->op_type == PGCLONE_OP_SCHEMA ||
             job->op_type == PGCLONE_OP_DATABASE)
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

        /* Clone sequences first (tables may depend on them for DEFAULT values) */
        {
            PGresult *seq_res;
            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "SELECT sequence_name FROM information_schema.sequences "
                "WHERE sequence_schema = '%s'",
                job->schema_name);

            seq_res = PQexec(source_conn, buf.data);
            if (PQresultStatus(seq_res) == PGRES_TUPLES_OK)
            {
                int si;
                for (si = 0; si < PQntuples(seq_res); si++)
                {
                    const char *seqname = PQgetvalue(seq_res, si, 0);
                    resetStringInfo(&buf);
                    appendStringInfo(&buf,
                        "CREATE SEQUENCE IF NOT EXISTS %s.%s",
                        job->schema_name, seqname);
                    bgw_exec(local_conn, buf.data);
                }
            }
            PQclear(seq_res);
        }

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
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            job->status = PGCLONE_JOB_FAILED;
            strlcpy(job->error_message, "could not list tables", 256);
            job->end_time = GetCurrentTimestamp();
            LWLockRelease(pgclone_state->lock);
            pfree(buf.data);
            goto cleanup;
        }

        ntables = PQntuples(table_res);

        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        job->total_tables = ntables;
        LWLockRelease(pgclone_state->lock);

        /* Resume support: skip tables before checkpoint */
        past_checkpoint = (job->resume_checkpoint[0] == '\0');

        for (i = 0; i < ntables; i++)
        {
            const char *tname = PQgetvalue(table_res, i, 0);

            /* Check for cancellation */
            LWLockAcquire(pgclone_state->lock, LW_SHARED);
            if (job->status == PGCLONE_JOB_CANCELLED)
            {
                LWLockRelease(pgclone_state->lock);
                PQclear(table_res);
                pfree(buf.data);
                goto cleanup;
            }
            LWLockRelease(pgclone_state->lock);

            /* Resume logic: skip until we pass the checkpoint */
            if (!past_checkpoint)
            {
                if (strcmp(tname, job->resume_checkpoint) == 0)
                    past_checkpoint = true;
                continue;
            }

            if (!bgw_clone_one_table(source_conn, local_conn, job, tname))
            {
                elog(WARNING, "pgclone bgw: failed to clone table %s, continuing...",
                     tname);
            }

            CHECK_FOR_INTERRUPTS();
        }

        PQclear(table_res);
        pfree(buf.data);
    }

    /* Mark job as completed */
    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
    if (job->status == PGCLONE_JOB_RUNNING)
        job->status = PGCLONE_JOB_COMPLETED;
    job->end_time = GetCurrentTimestamp();
    strlcpy(job->current_phase, "completed", 64);
    LWLockRelease(pgclone_state->lock);

cleanup:
    if (source_conn)
        PQfinish(source_conn);
    if (local_conn)
        PQfinish(local_conn);

    proc_exit(0);
}

/* ===============================================================
 * Pool worker main function
 *
 * Each pool worker grabs tasks from the shared queue one at a time.
 * The job_id passed via bgw_main_arg is this worker's own tracking
 * job in the jobs[] array; the pool queue holds the task list.
 * =============================================================== */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
void
pgclone_pool_worker_main(Datum main_arg)
{
    int             job_id = DatumGetInt32(main_arg);
    PgcloneJob     *job;
    PGconn         *source_conn = NULL;
    PGconn         *local_conn = NULL;
    const char     *port;
    const char     *dbname;

    /* Signal handlers */
#if PG_VERSION_NUM >= 170000
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
#else
    pqsignal(SIGTERM, die);
#endif
    BackgroundWorkerUnblockSignals();

    if (!pgclone_state)
    {
        elog(ERROR, "pgclone pool worker: shared memory not initialized");
        return;
    }

    /* Find our job and mark as running */
    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
    job = find_job(job_id);
    if (!job || job->status != PGCLONE_JOB_PENDING)
    {
        LWLockRelease(pgclone_state->lock);
        elog(ERROR, "pgclone pool worker: job %d not found or not pending", job_id);
        return;
    }
    job->status = PGCLONE_JOB_RUNNING;
    job->worker_pid = MyProcPid;
    job->start_time = GetCurrentTimestamp();
    LWLockRelease(pgclone_state->lock);

    /* Initialize database connection */
    BackgroundWorkerInitializeConnectionByOid(job->database_oid, InvalidOid, 0);

    dbname = job->database_name;
    port = GetConfigOption("port", false, false);

    /* Connect to source and local once — reuse for all tasks */
    source_conn = PQconnectdb(pgclone_state->pool.source_conninfo);
    if (PQstatus(source_conn) != CONNECTION_OK)
    {
        elog(WARNING, "pgclone pool worker: source connection failed: %s",
             PQerrorMessage(source_conn));
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        job->status = PGCLONE_JOB_FAILED;
        strlcpy(job->error_message, "could not connect to source", 256);
        job->end_time = GetCurrentTimestamp();
        LWLockRelease(pgclone_state->lock);
        goto pool_cleanup;
    }

    /* Pin source search_path — see comment in pgclone_bgw_main(). */
    {
        PGresult *sp_res = PQexec(source_conn, "SET search_path = pg_catalog");
        if (PQresultStatus(sp_res) != PGRES_COMMAND_OK)
            elog(WARNING, "pgclone pool worker: could not set source search_path: %s",
                 PQerrorMessage(source_conn));
        PQclear(sp_res);
    }

    local_conn = bgw_connect_local(dbname, port, job->username);
    if (!local_conn)
    {
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        job->status = PGCLONE_JOB_FAILED;
        strlcpy(job->error_message, "could not connect to local database", 256);
        job->end_time = GetCurrentTimestamp();
        LWLockRelease(pgclone_state->lock);
        goto pool_cleanup;
    }

    /* Main loop: grab tasks from pool queue until exhausted */
    for (;;)
    {
        int             task_idx;
        char            table_name[NAMEDATALEN];

        CHECK_FOR_INTERRUPTS();

        /* Check for cancellation */
        LWLockAcquire(pgclone_state->lock, LW_SHARED);
        if (job->status == PGCLONE_JOB_CANCELLED)
        {
            LWLockRelease(pgclone_state->lock);
            break;
        }
        LWLockRelease(pgclone_state->lock);

        /* Atomically claim next task */
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

        if (pgclone_state->pool.next_task_idx >= pgclone_state->pool.num_tasks)
        {
            /* No more tasks */
            LWLockRelease(pgclone_state->lock);
            break;
        }

        task_idx = pgclone_state->pool.next_task_idx++;
        pgclone_state->pool.tasks[task_idx].status = 1; /* in_progress */
        pgclone_state->pool.tasks[task_idx].claimed_by_job_id = job_id;
        strlcpy(table_name, pgclone_state->pool.tasks[task_idx].table_name, NAMEDATALEN);

        /* Update worker job progress */
        strlcpy(job->current_table, table_name, NAMEDATALEN);
        strlcpy(job->current_phase, "cloning table", 64);

        LWLockRelease(pgclone_state->lock);

        elog(DEBUG1, "pgclone pool worker %d: claiming task %d (%s)",
             job_id, task_idx, table_name);

        /* Populate job fields needed by bgw_clone_one_table */
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        strlcpy(job->table_name, table_name, NAMEDATALEN);
        strlcpy(job->target_name, table_name, NAMEDATALEN);
        strlcpy(job->source_conninfo,
                pgclone_state->pool.source_conninfo,
                sizeof(job->source_conninfo));
        strlcpy(job->schema_name,
                pgclone_state->pool.schema_name, NAMEDATALEN);
        job->include_data = pgclone_state->pool.include_data;
        job->include_indexes = pgclone_state->pool.include_indexes;
        job->include_constraints = pgclone_state->pool.include_constraints;
        job->include_triggers = pgclone_state->pool.include_triggers;
        job->conflict_strategy = pgclone_state->pool.conflict_strategy;
        job->copied_rows = 0;
        LWLockRelease(pgclone_state->lock);

        /* Clone the table */
        if (bgw_clone_one_table(source_conn, local_conn, job, table_name))
        {
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            pgclone_state->pool.tasks[task_idx].status = 2; /* done */
            pgclone_state->pool.completed_count++;
            job->completed_tables++;

            /* Update parent job aggregate progress */
            {
                PgcloneJob *parent = find_job(pgclone_state->pool.parent_job_id);
                if (parent)
                {
                    parent->completed_tables = pgclone_state->pool.completed_count;
                    snprintf(parent->current_phase, 64, "%d/%d tables done",
                             pgclone_state->pool.completed_count,
                             pgclone_state->pool.num_tasks);
                }
            }
            LWLockRelease(pgclone_state->lock);
        }
        else
        {
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            pgclone_state->pool.tasks[task_idx].status = 3; /* failed */
            pgclone_state->pool.failed_count++;

            /* Still update parent progress */
            {
                PgcloneJob *parent = find_job(pgclone_state->pool.parent_job_id);
                if (parent)
                {
                    parent->completed_tables = pgclone_state->pool.completed_count;
                    snprintf(parent->current_phase, 64, "%d/%d tables done (%d failed)",
                             pgclone_state->pool.completed_count,
                             pgclone_state->pool.num_tasks,
                             pgclone_state->pool.failed_count);
                }
            }
            LWLockRelease(pgclone_state->lock);

            elog(WARNING, "pgclone pool worker %d: failed to clone table %s, continuing...",
                 job_id, table_name);
        }
    }

    /* Mark this worker job as completed */
    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
    if (job->status == PGCLONE_JOB_RUNNING)
        job->status = PGCLONE_JOB_COMPLETED;
    job->end_time = GetCurrentTimestamp();
    strlcpy(job->current_phase, "completed", 64);

    /* Check if all pool workers are done — if so, finalize parent */
    {
        PgcloneJob *parent = find_job(pgclone_state->pool.parent_job_id);
        if (parent && parent->status == PGCLONE_JOB_RUNNING)
        {
            bool all_done = true;
            int i;

            for (i = 0; i < pgclone_state->pool.num_workers; i++)
            {
                PgcloneJob *w = find_job(pgclone_state->pool.worker_job_ids[i]);
                if (w && (w->status == PGCLONE_JOB_RUNNING ||
                          w->status == PGCLONE_JOB_PENDING))
                {
                    all_done = false;
                    break;
                }
            }

            if (all_done)
            {
                parent->status = (pgclone_state->pool.failed_count > 0)
                    ? PGCLONE_JOB_FAILED : PGCLONE_JOB_COMPLETED;
                parent->end_time = GetCurrentTimestamp();
                snprintf(parent->current_phase, 64, "completed (%d/%d ok)",
                         pgclone_state->pool.completed_count,
                         pgclone_state->pool.num_tasks);
                pgclone_state->pool.active = false;
            }
        }
    }
    LWLockRelease(pgclone_state->lock);

pool_cleanup:
    if (source_conn)
        PQfinish(source_conn);
    if (local_conn)
        PQfinish(local_conn);

    proc_exit(0);
}
