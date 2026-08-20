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
#include "utils/wait_event.h"
#include "commands/dbcommands.h"
#include "utils/guc.h"
#include "libpq-fe.h"

#include "pgclone_compat.h"
#include "pgclone_bgw.h"
#include "pgclone_snapshot.h"

/* Shared memory state */
PgcloneSharedState *pgclone_state = NULL;

/* Shmem hook chain */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

#if PGCLONE_HAS_SHMEM_REQUEST_HOOK
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
#if PGCLONE_HAS_SHMEM_REQUEST_HOOK
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();
#endif

    RequestAddinShmemSpace(pgclone_shmem_size());
    RequestNamedLWLockTranche("pgclone", 1);
}

void
pgclone_shmem_init(void)
{
#if PGCLONE_HAS_SHMEM_REQUEST_HOOK
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
 * v4.3.2 source-side conninfo augmentation (bgw mirror of
 * pgclone_connect_with_keepalives in pgclone.c).
 *
 * Background-worker source connections are subject to the same
 * idle-keeper failure modes as the synchronous path (issue #9):
 * firewall/NAT idle TCP drops kill the long-running pool
 * coordinator's keeper, and the single-job worker's source
 * connection can drop during slow COPYs. Parse the user's conninfo
 * with PQconninfoParse, inject TCP keepalive defaults only when the
 * user did not set them, and connect via PQconnectdbParams. URI
 * and keyword-form conninfo strings are both handled.
 *
 * Failure to parse falls through to plain PQconnectdb so the
 * caller's existing PQstatus()-based failure path surfaces a
 * normal connection error rather than a hard worker-abort.
 * --------------------------------------------------------------- */
static PGconn *
bgw_connect_with_keepalives(const char *conninfo)
{
    PQconninfoOption *parsed;
    PQconninfoOption *opt;
    char             *parse_err = NULL;
    const char      **keywords;
    const char      **values;
    int               nopts = 0;
    int               i;
    bool              have_keepalives          = false;
    bool              have_keepalives_idle     = false;
    bool              have_keepalives_interval = false;
    bool              have_keepalives_count    = false;
    PGconn           *conn;

    parsed = PQconninfoParse(conninfo, &parse_err);
    if (parsed == NULL)
    {
        elog(WARNING, "pgclone bgw: could not parse conninfo (%s); "
                      "falling back to plain PQconnectdb",
             parse_err ? parse_err : "unknown");
        if (parse_err)
            PQfreemem(parse_err);
        return PQconnectdb(conninfo);
    }

    for (opt = parsed; opt->keyword != NULL; opt++)
    {
        if (opt->val != NULL && opt->val[0] != '\0')
        {
            nopts++;
            if (strcmp(opt->keyword, "keepalives") == 0)
                have_keepalives = true;
            else if (strcmp(opt->keyword, "keepalives_idle") == 0)
                have_keepalives_idle = true;
            else if (strcmp(opt->keyword, "keepalives_interval") == 0)
                have_keepalives_interval = true;
            else if (strcmp(opt->keyword, "keepalives_count") == 0)
                have_keepalives_count = true;
        }
    }

    keywords = (const char **) palloc0(sizeof(char *) * (nopts + 5));
    values   = (const char **) palloc0(sizeof(char *) * (nopts + 5));

    i = 0;
    for (opt = parsed; opt->keyword != NULL; opt++)
    {
        if (opt->val != NULL && opt->val[0] != '\0')
        {
            keywords[i] = pstrdup(opt->keyword);
            values[i]   = pstrdup(opt->val);
            i++;
        }
    }

    if (!have_keepalives)          { keywords[i] = "keepalives";          values[i++] = "1";  }
    if (!have_keepalives_idle)     { keywords[i] = "keepalives_idle";     values[i++] = "30"; }
    if (!have_keepalives_interval) { keywords[i] = "keepalives_interval"; values[i++] = "10"; }
    if (!have_keepalives_count)    { keywords[i] = "keepalives_count";    values[i++] = "6";  }

    keywords[i] = NULL;
    values[i]   = NULL;

    PQconninfoFree(parsed);

    conn = PQconnectdbParams(keywords, values, 0);

    pfree(keywords);
    pfree(values);

    return conn;
}

/* ---------------------------------------------------------------
 * v4.3.0 Source-side snapshot helpers.
 *
 * Bgw callers used to have their own copy of these; both paths now
 * share the implementation in pgclone_snapshot.c. Local one-liner
 * wrappers keep the WARNING-then-abort pattern the callers below
 * expect, without dragging that policy into the shared helper.
 * --------------------------------------------------------------- */
static bool
bgw_begin_repeatable_read(PGconn *conn)
{
    if (pgclone_snap_begin_repeatable_read(conn))
        return true;
    elog(WARNING, "pgclone bgw: BEGIN REPEATABLE READ failed: %s",
         PQerrorMessage(conn));
    return false;
}

static void
bgw_commit_source(PGconn *conn)
{
    pgclone_snap_commit_source(conn);
}

static bool
bgw_export_snapshot(PGconn *conn, char *out_id, size_t out_id_len)
{
    if (pgclone_snap_export(conn, out_id, out_id_len))
        return true;
    elog(WARNING, "pgclone bgw: pg_export_snapshot failed: %s",
         PQerrorMessage(conn));
    return false;
}

static bool
bgw_begin_with_imported_snapshot(PGconn *conn, const char *snapshot_id)
{
    const char *errtxt;

    if (pgclone_snap_import(conn, snapshot_id))
        return true;

    errtxt = PQerrorMessage(conn);
    if (errtxt != NULL && strstr(errtxt, "invalid snapshot identifier"))
        elog(WARNING,
             "pgclone bgw: SET TRANSACTION SNAPSHOT '%s' failed: %s"
             "HINT: The exporting (coordinator/keeper) transaction "
             "was likely terminated. Common causes: firewall idle drop, "
             "idle_in_transaction_session_timeout on the source, or — on "
             "a PostgreSQL 17+ source — a non-zero transaction_timeout "
             "(caps total transaction age, fires even on an active "
             "keeper). Async path mitigations: TCP keepalives are "
             "auto-injected; the keeper transaction issues SET LOCAL "
             "idle_in_transaction_session_timeout = 0, statement_timeout "
             "= 0, and (PG 17+) transaction_timeout = 0. "
             "Emergency workaround: pass {\"consistent\": false} in "
             "the options JSON. See issue #5 / #9.",
             snapshot_id, errtxt);
    else
        elog(WARNING,
             "pgclone bgw: SET TRANSACTION SNAPSHOT '%s' failed: %s",
             snapshot_id, errtxt ? errtxt : "(no error message)");
    return false;
}

static bool
bgw_keeper_ping(PGconn *conn)
{
    if (pgclone_snap_keeper_ping(conn))
        return true;
    elog(WARNING,
         "pgclone bgw: snapshot keeper ping failed: %s",
         PQerrorMessage(conn));
    return false;
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
 * Unexpected-exit cleanup for the three bgw entry points.
 *
 * A worker that ereport(ERROR)s past its normal cleanup: label, is
 * SIGTERM'd, or otherwise proc_exit's without walking its own
 * finalize block leaves its jobs[] slot RUNNING forever, and the
 * pool parent job can hang because "last worker finalizes parent"
 * never fires. A before_shmem_exit callback runs on every path
 * that reaches proc_exit — including uncaught ERROR and shutdown
 * requests — and still has access to shared memory.
 *
 * Registered once per bgw entry after signal setup. Idempotent: the
 * normal COMPLETED/FAILED transition leaves this a no-op.
 * (issue: P0-2 in the 4.4.2 review.)
 * =============================================================== */
typedef enum PgcloneBgwRole
{
    PGCLONE_BGW_ROLE_NONE = 0,
    PGCLONE_BGW_ROLE_SINGLE,
    PGCLONE_BGW_ROLE_POOL_WORKER,
    PGCLONE_BGW_ROLE_POOL_COORD
} PgcloneBgwRole;

static int             pgclone_my_job_id = 0;
static PgcloneBgwRole  pgclone_my_role   = PGCLONE_BGW_ROLE_NONE;

static void
pgclone_bgw_exit_cleanup(int code, Datum arg)
{
    PgcloneJob *job;

    if (pgclone_state == NULL || pgclone_my_job_id == 0)
        return;

    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

    job = find_job(pgclone_my_job_id);
    if (job != NULL &&
        (job->status == PGCLONE_JOB_RUNNING ||
         job->status == PGCLONE_JOB_PENDING))
    {
        job->status = PGCLONE_JOB_FAILED;
        if (job->error_message[0] == '\0')
            strlcpy(job->error_message,
                    "worker exited unexpectedly",
                    sizeof(job->error_message));
        job->end_time = GetCurrentTimestamp();
        strlcpy(job->current_phase,
                "worker exited unexpectedly",
                sizeof(job->current_phase));
    }

    /* A pool coordinator that dies mid-publish must unblock any
     * worker still waiting on snapshot_ready. Safe to set after
     * publish too — pool workers hold their own snapshot by then. */
    if (pgclone_my_role == PGCLONE_BGW_ROLE_POOL_COORD)
        pgclone_state->pool.snapshot_failed = true;

    /* A pool worker that dies without walking its finalize block
     * would leave the parent job RUNNING forever if it was the last
     * live worker. Repeat the "all done → finalize parent" check. */
    if (pgclone_my_role == PGCLONE_BGW_ROLE_POOL_WORKER &&
        pgclone_state->pool.active)
    {
        PgcloneJob *parent = find_job(pgclone_state->pool.parent_job_id);
        if (parent != NULL && parent->status == PGCLONE_JOB_RUNNING)
        {
            bool all_done = true;
            int  i;

            for (i = 0; i < pgclone_state->pool.num_workers; i++)
            {
                PgcloneJob *w = find_job(pgclone_state->pool.worker_job_ids[i]);
                if (w != NULL &&
                    (w->status == PGCLONE_JOB_RUNNING ||
                     w->status == PGCLONE_JOB_PENDING))
                {
                    all_done = false;
                    break;
                }
            }

            if (all_done)
            {
                bool any_failed =
                    (pgclone_state->pool.failed_count > 0) ||
                    (pgclone_state->pool.completed_count <
                     pgclone_state->pool.num_tasks);

                parent->status = any_failed
                    ? PGCLONE_JOB_FAILED
                    : PGCLONE_JOB_COMPLETED;
                parent->end_time = GetCurrentTimestamp();
                snprintf(parent->current_phase, 64,
                         "completed (%d/%d ok)",
                         pgclone_state->pool.completed_count,
                         pgclone_state->pool.num_tasks);
                pgclone_state->pool.active = false;
            }
        }
    }

    LWLockRelease(pgclone_state->lock);
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

    pqsignal(SIGTERM, pgclone_shutdown_handler);
    BackgroundWorkerUnblockSignals();

    pgclone_my_job_id = job_id;
    pgclone_my_role   = PGCLONE_BGW_ROLE_SINGLE;
    before_shmem_exit(pgclone_bgw_exit_cleanup, (Datum) 0);

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


    source_conn = bgw_connect_with_keepalives(job->source_conninfo);
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

    /* v4.3.0: Wrap the source connection in a REPEATABLE READ READ
     * ONLY transaction so every per-table COPY in this single-worker
     * clone sees the same snapshot. Cross-table FK consistency
     * requires this — without it, schema_async on a live source can
     * observe FK violations between independently-snapshot-ed
     * COPY commands. */
    if (job->consistent)
    {
        if (!bgw_begin_repeatable_read(source_conn))
        {
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            job->status = PGCLONE_JOB_FAILED;
            strlcpy(job->error_message,
                    "could not start REPEATABLE READ transaction on source", 256);
            job->end_time = GetCurrentTimestamp();
            LWLockRelease(pgclone_state->lock);
            goto cleanup;
        }
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
                "SELECT s.relname, "
                "       pg_sequence.seqstart, "
                "       pg_sequence.seqincrement, "
                "       pg_sequence.seqmax, "
                "       pg_sequence.seqmin, "
                "       pg_sequence.seqcache, "
                "       pg_sequence.seqcycle, "
                "       pg_sequence.seqtypid::regtype::text "
                "FROM pg_catalog.pg_class s "
                "JOIN pg_catalog.pg_namespace n ON n.oid = s.relnamespace "
                "JOIN pg_catalog.pg_sequence ON pg_sequence.seqrelid = s.oid "
                "WHERE n.nspname = %s AND s.relkind = 'S'",
                quote_literal_cstr(job->schema_name));

            seq_res = PQexec(source_conn, buf.data);
            if (PQresultStatus(seq_res) == PGRES_TUPLES_OK)
            {
                int si;
                for (si = 0; si < PQntuples(seq_res); si++)
                {
                    resetStringInfo(&buf);
                    appendStringInfo(&buf,
                        "CREATE SEQUENCE IF NOT EXISTS %s.%s "
                        "AS %s "
                        "START WITH %s INCREMENT BY %s "
                        "MINVALUE %s MAXVALUE %s CACHE %s %s",
                        quote_identifier(job->schema_name),
                        quote_identifier(PQgetvalue(seq_res, si, 0)),
                        PQgetvalue(seq_res, si, 7),   /* data type */
                        PQgetvalue(seq_res, si, 1),   /* start */
                        PQgetvalue(seq_res, si, 2),   /* increment */
                        PQgetvalue(seq_res, si, 4),   /* min */
                        PQgetvalue(seq_res, si, 3),   /* max */
                        PQgetvalue(seq_res, si, 5),   /* cache */
                        strcmp(PQgetvalue(seq_res, si, 6), "t") == 0 ?
                            "CYCLE" : "NO CYCLE");
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

        /* Sync sequence current values after all table data is copied.
         * CREATE SEQUENCE only records the definition; the runtime
         * position must be replayed via setval() to prevent ID reuse. */
        if (job->include_data)
        {
            PGresult   *sv_res;
            StringInfoData svbuf;

            initStringInfo(&svbuf);
            appendStringInfo(&svbuf,
                "SELECT sequencename "
                "FROM pg_catalog.pg_sequences "
                "WHERE schemaname = %s "
                "AND last_value IS NOT NULL",
                quote_literal_cstr(job->schema_name));

            sv_res = PQexec(source_conn, svbuf.data);
            if (PQresultStatus(sv_res) == PGRES_TUPLES_OK)
            {
                int si;
                for (si = 0; si < PQntuples(sv_res); si++)
                {
                    const char *seqname = PQgetvalue(sv_res, si, 0);
                    char       *qualified;
                    const char *quoted_seq;
                    PGresult   *val_res;

                    qualified  = psprintf("%s.%s",
                                          quote_identifier(job->schema_name),
                                          quote_identifier(seqname));
                    quoted_seq = quote_literal_cstr(qualified);

                    resetStringInfo(&svbuf);
                    appendStringInfo(&svbuf,
                        "SELECT last_value, is_called FROM %s", qualified);
                    val_res = PQexec(source_conn, svbuf.data);

                    if (PQresultStatus(val_res) == PGRES_TUPLES_OK &&
                        PQntuples(val_res) == 1)
                    {
                        const char *last_val  = PQgetvalue(val_res, 0, 0);
                        const char *is_called = PQgetvalue(val_res, 0, 1);

                        resetStringInfo(&svbuf);
                        appendStringInfo(&svbuf,
                            "SELECT setval(%s, %s, %s)",
                            quoted_seq,
                            last_val,
                            strcmp(is_called, "t") == 0 ? "true" : "false");

                        bgw_exec(local_conn, svbuf.data);
                    }
                    PQclear(val_res);
                    pfree(qualified);
                }
                elog(DEBUG1,
                     "pgclone bgw: synced current value for %d sequences in schema %s",
                     PQntuples(sv_res), job->schema_name);
            }
            PQclear(sv_res);
            pfree(svbuf.data);
        }

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
    {
        bgw_commit_source(source_conn);
        PQfinish(source_conn);
    }
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
    bool            is_last_worker = false;
    int             pool_failed_count = 0;

    pqsignal(SIGTERM, pgclone_shutdown_handler);
    BackgroundWorkerUnblockSignals();

    pgclone_my_job_id = job_id;
    pgclone_my_role   = PGCLONE_BGW_ROLE_POOL_WORKER;
    before_shmem_exit(pgclone_bgw_exit_cleanup, (Datum) 0);

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
    source_conn = bgw_connect_with_keepalives(pgclone_state->pool.source_conninfo);
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

    /* v4.3.0: When the pool runs in consistent mode, the coordinator
     * has BEGIN'd a REPEATABLE READ READ ONLY transaction on its own
     * source connection and called pg_export_snapshot(). We wait for
     * that snapshot to be published, then import it on this worker's
     * source connection so every COPY across every worker reads the
     * same point-in-time snapshot. This is the same pattern pg_dump -j
     * uses for parallel dump consistency. */
    if (pgclone_state->pool.consistent)
    {
        bool ready = false;
        bool failed = false;
        char snap_buf[64];
        int  attempts;

        snap_buf[0] = '\0';
        for (attempts = 0; attempts < 600; attempts++)  /* up to ~60s */
        {
            CHECK_FOR_INTERRUPTS();
            LWLockAcquire(pgclone_state->lock, LW_SHARED);
            if (pgclone_state->pool.snapshot_failed)
                failed = true;
            else if (pgclone_state->pool.snapshot_ready)
            {
                strlcpy(snap_buf, pgclone_state->pool.snapshot_id, sizeof(snap_buf));
                ready = true;
            }
            if (job->status == PGCLONE_JOB_CANCELLED)
                failed = true;
            LWLockRelease(pgclone_state->lock);

            if (ready || failed)
                break;

            (void) WaitLatch(MyLatch,
                             WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                             100L,
                             PG_WAIT_EXTENSION);
            ResetLatch(MyLatch);
        }

        if (!ready || failed)
        {
            elog(WARNING, "pgclone pool worker: snapshot %s",
                 failed ? "publish failed" : "publish timed out");
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            job->status = PGCLONE_JOB_FAILED;
            strlcpy(job->error_message,
                    failed ? "snapshot coordinator reported failure"
                           : "timed out waiting for snapshot",
                    256);
            job->end_time = GetCurrentTimestamp();
            LWLockRelease(pgclone_state->lock);
            goto pool_cleanup;
        }

        if (!bgw_begin_with_imported_snapshot(source_conn, snap_buf))
        {
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            job->status = PGCLONE_JOB_FAILED;
            strlcpy(job->error_message,
                    "could not import shared snapshot on source", 256);
            job->end_time = GetCurrentTimestamp();
            /* Signal coordinator to abort cleanly */
            pgclone_state->pool.snapshot_failed = true;
            LWLockRelease(pgclone_state->lock);
            goto pool_cleanup;
        }

        /* Tell the coordinator we're now bound to its snapshot.
         * Once imported_count reaches num_workers the coordinator may
         * COMMIT and exit; our own transaction owns the snapshot. */
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        pgclone_state->pool.snapshot_imported_count++;
        LWLockRelease(pgclone_state->lock);
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
                is_last_worker = true;
                pool_failed_count = pgclone_state->pool.failed_count;
            }
        }
    }
    LWLockRelease(pgclone_state->lock);

    /*
     * Last worker to finish syncs sequence current values.
     *
     * CREATE SEQUENCE in the pre-launch phase (pgclone_schema_async) only
     * sets the definition.  The actual runtime position — how far nextval()
     * has already advanced — must be replayed via setval() so the target
     * never re-issues IDs that already exist in the just-copied data.
     *
     * We skip this if any table failed: the dataset is incomplete and the
     * caller must handle the inconsistency.
     */
    if (is_last_worker && pool_failed_count == 0)
    {
        PGconn     *sv_source = NULL;
        PGconn     *sv_local  = NULL;
        PGresult   *sv_res;
        StringInfoData svbuf;
        int         si;
        char        schema_name_copy[NAMEDATALEN];
        char        source_conninfo_copy[1024];

        /* Copy from shared memory before using outside the lock */
        strlcpy(schema_name_copy,    pgclone_state->pool.schema_name,    NAMEDATALEN);
        strlcpy(source_conninfo_copy, pgclone_state->pool.source_conninfo,
                sizeof(source_conninfo_copy));

        sv_source = bgw_connect_with_keepalives(source_conninfo_copy);
        sv_local  = bgw_connect_local(dbname, port, job->username);

        if (PQstatus(sv_source) == CONNECTION_OK &&
            PQstatus(sv_local)  == CONNECTION_OK)
        {
            initStringInfo(&svbuf);
            appendStringInfo(&svbuf,
                "SELECT sequencename "
                "FROM pg_catalog.pg_sequences "
                "WHERE schemaname = %s "
                "AND last_value IS NOT NULL",
                quote_literal_cstr(schema_name_copy));

            sv_res = PQexec(sv_source, svbuf.data);

            if (PQresultStatus(sv_res) == PGRES_TUPLES_OK)
            {
                for (si = 0; si < PQntuples(sv_res); si++)
                {
                    const char *seqname = PQgetvalue(sv_res, si, 0);
                    char       *qualified;
                    const char *quoted_seq;
                    PGresult   *val_res;

                    qualified  = psprintf("%s.%s",
                                          quote_identifier(schema_name_copy),
                                          quote_identifier(seqname));
                    quoted_seq = quote_literal_cstr(qualified);

                    resetStringInfo(&svbuf);
                    appendStringInfo(&svbuf,
                        "SELECT last_value, is_called FROM %s", qualified);
                    val_res = PQexec(sv_source, svbuf.data);

                    if (PQresultStatus(val_res) == PGRES_TUPLES_OK &&
                        PQntuples(val_res) == 1)
                    {
                        const char *last_val  = PQgetvalue(val_res, 0, 0);
                        const char *is_called = PQgetvalue(val_res, 0, 1);
                        PGresult   *lcres;

                        resetStringInfo(&svbuf);
                        appendStringInfo(&svbuf,
                            "SELECT setval(%s, %s, %s)",
                            quoted_seq,
                            last_val,
                            strcmp(is_called, "t") == 0 ? "true" : "false");

                        lcres = PQexec(sv_local, svbuf.data);
                        if (PQresultStatus(lcres) != PGRES_TUPLES_OK)
                            elog(WARNING,
                                 "pgclone pool: setval for sequence %s.%s failed: %s",
                                 schema_name_copy, seqname,
                                 PQerrorMessage(sv_local));
                        PQclear(lcres);
                    }
                    PQclear(val_res);
                    pfree(qualified);
                }

                elog(DEBUG1,
                     "pgclone pool: synced current value for %d sequences in schema %s",
                     PQntuples(sv_res), schema_name_copy);
            }
            else
            {
                elog(WARNING,
                     "pgclone pool: could not query pg_sequences for schema %s: %s",
                     schema_name_copy, PQerrorMessage(sv_source));
            }

            PQclear(sv_res);
            pfree(svbuf.data);
        }
        else
        {
            elog(WARNING, "pgclone pool: could not open connections for setval pass");
        }

        if (sv_source) PQfinish(sv_source);
        if (sv_local)  PQfinish(sv_local);
    }

pool_cleanup:
    if (source_conn)
    {
        bgw_commit_source(source_conn);
        PQfinish(source_conn);
    }
    if (local_conn)
        PQfinish(local_conn);

    proc_exit(0);
}

/* ===============================================================
 * Pool snapshot coordinator
 *
 * A short-lived bgworker that exists only to hold the source-side
 * REPEATABLE READ READ ONLY transaction whose snapshot every pool
 * worker imports. It connects to source, BEGIN's, calls
 * pg_export_snapshot(), publishes the ID into shared memory, and
 * then sits idle in transaction until every worker has imported.
 * Once imported_count == num_workers (or a timeout / failure /
 * cancellation fires) it COMMITs and exits. The keeper transaction
 * MUST stay alive at least until each importer has issued
 * SET TRANSACTION SNAPSHOT — after that, importers' own transactions
 * own the snapshot independently, and the keeper is free to release.
 *
 * The job_id passed in main_arg is a tracking job slot in jobs[]
 * (similar to a regular pool worker). Status transitions mirror
 * pgclone_pool_worker_main(), so existing progress queries continue
 * to work.
 * =============================================================== */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
void
pgclone_pool_coordinator_main(Datum main_arg)
{
    int             job_id = DatumGetInt32(main_arg);
    PgcloneJob     *job;
    PGconn         *source_conn = NULL;

    pqsignal(SIGTERM, pgclone_shutdown_handler);
    BackgroundWorkerUnblockSignals();

    pgclone_my_job_id = job_id;
    pgclone_my_role   = PGCLONE_BGW_ROLE_POOL_COORD;
    before_shmem_exit(pgclone_bgw_exit_cleanup, (Datum) 0);

    if (!pgclone_state)
    {
        elog(ERROR, "pgclone pool coordinator: shared memory not initialized");
        return;
    }

    /* Find tracking job and mark running */
    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
    job = find_job(job_id);
    if (!job || job->status != PGCLONE_JOB_PENDING)
    {
        LWLockRelease(pgclone_state->lock);
        elog(ERROR, "pgclone pool coordinator: job %d not found or not pending",
             job_id);
        return;
    }
    job->status = PGCLONE_JOB_RUNNING;
    job->worker_pid = MyProcPid;
    job->start_time = GetCurrentTimestamp();
    strlcpy(job->current_phase, "exporting snapshot", 64);
    LWLockRelease(pgclone_state->lock);

    /* Required for any libpq operation in a bgworker context */
    BackgroundWorkerInitializeConnectionByOid(job->database_oid, InvalidOid, 0);

    source_conn = bgw_connect_with_keepalives(pgclone_state->pool.source_conninfo);
    if (PQstatus(source_conn) != CONNECTION_OK)
    {
        elog(WARNING, "pgclone pool coordinator: source connection failed: %s",
             PQerrorMessage(source_conn));
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        pgclone_state->pool.snapshot_failed = true;
        job->status = PGCLONE_JOB_FAILED;
        strlcpy(job->error_message, "could not connect to source", 256);
        job->end_time = GetCurrentTimestamp();
        LWLockRelease(pgclone_state->lock);
        goto coord_cleanup;
    }

    /* Pin source search_path — same rationale as bgw_main. */
    {
        PGresult *sp_res = PQexec(source_conn, "SET search_path = pg_catalog");
        PQclear(sp_res);
    }

    if (!bgw_begin_repeatable_read(source_conn))
    {
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        pgclone_state->pool.snapshot_failed = true;
        job->status = PGCLONE_JOB_FAILED;
        strlcpy(job->error_message,
                "could not BEGIN REPEATABLE READ on source", 256);
        job->end_time = GetCurrentTimestamp();
        LWLockRelease(pgclone_state->lock);
        goto coord_cleanup;
    }

    {
        char snap[64];

        if (!bgw_export_snapshot(source_conn, snap, sizeof(snap)))
        {
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            pgclone_state->pool.snapshot_failed = true;
            job->status = PGCLONE_JOB_FAILED;
            strlcpy(job->error_message,
                    "pg_export_snapshot failed on source", 256);
            job->end_time = GetCurrentTimestamp();
            LWLockRelease(pgclone_state->lock);
            goto coord_cleanup;
        }

        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        strlcpy(pgclone_state->pool.snapshot_id, snap,
                sizeof(pgclone_state->pool.snapshot_id));
        pgclone_state->pool.snapshot_ready = true;
        strlcpy(job->current_phase, "holding snapshot", 64);
        LWLockRelease(pgclone_state->lock);

        elog(DEBUG1, "pgclone pool coordinator: published snapshot %s", snap);
    }

    /* Wait until every pool worker has imported the snapshot, then
     * COMMIT to release the keeper transaction. The foreground sets
     * snapshot_expected_workers (and launch_complete) once all
     * worker bgworkers have been registered. If a worker fails to
     * import it sets snapshot_failed and we abort, which cancels
     * every other worker's wait loop as well. Cap at ~10 minutes —
     * far longer than any reasonable startup. */
    {
        int attempts;

        for (attempts = 0; attempts < 6000; attempts++)  /* up to ~10min */
        {
            int  imported;
            int  target;
            bool failed;
            bool cancelled;
            bool launch_done;

            CHECK_FOR_INTERRUPTS();

            LWLockAcquire(pgclone_state->lock, LW_SHARED);
            imported    = pgclone_state->pool.snapshot_imported_count;
            target      = pgclone_state->pool.snapshot_expected_workers;
            failed      = pgclone_state->pool.snapshot_failed;
            launch_done = pgclone_state->pool.launch_complete;
            cancelled   = (job->status == PGCLONE_JOB_CANCELLED);
            LWLockRelease(pgclone_state->lock);

            if (failed || cancelled)
            {
                LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
                pgclone_state->pool.snapshot_failed = true;
                LWLockRelease(pgclone_state->lock);
                break;
            }
            /* Only release the keeper after foreground has finished
             * launching every worker AND each launched worker has
             * imported. Releasing earlier would race with a still-
             * launching importer. */
            if (launch_done && imported >= target)
                break;

            /* v4.3.2: every ~5 s (50 × 100 ms WaitLatch) probe the
             * keeper to detect a silently-dropped TCP session or a
             * server-side termination BEFORE pool workers fail to
             * import. Keepalives already auto-detect dropped TCP
             * within ~90 s; this catches the residual paths where
             * the server actively closes the connection (e.g.
             * idle_in_transaction_session_timeout firing despite
             * our SET LOCAL, an external superuser-driven
             * pg_terminate_backend, or a wal_sender_timeout
             * variant). (issue #9 mirror in bgw) */
            if (attempts % 50 == 0 && attempts > 0 && !bgw_keeper_ping(source_conn))
            {
                LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
                pgclone_state->pool.snapshot_failed = true;
                strlcpy(job->error_message,
                        "snapshot keeper connection died — see WARNING above",
                        256);
                LWLockRelease(pgclone_state->lock);
                break;
            }

            (void) WaitLatch(MyLatch,
                             WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                             100L,
                             PG_WAIT_EXTENSION);
            ResetLatch(MyLatch);
        }
    }

    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
    if (job->status == PGCLONE_JOB_RUNNING)
        job->status = PGCLONE_JOB_COMPLETED;
    job->end_time = GetCurrentTimestamp();
    strlcpy(job->current_phase, "snapshot released", 64);
    LWLockRelease(pgclone_state->lock);

coord_cleanup:
    if (source_conn)
    {
        bgw_commit_source(source_conn);
        PQfinish(source_conn);
    }

    proc_exit(0);
}
