/*
 * pgclone_bgw.h - Background worker definitions for pgclone
 *
 * Defines the shared memory structures for job tracking,
 * progress reporting, and parallel clone coordination.
 */

#ifndef PGCLONE_BGW_H
#define PGCLONE_BGW_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

/* Maximum concurrent clone jobs */
#define PGCLONE_MAX_JOBS      16

/* Maximum tables in pool task queue */
#define PGCLONE_MAX_POOL_TASKS  512

/* Job status enum */
typedef enum PgcloneJobStatus
{
    PGCLONE_JOB_FREE = 0,     /* Slot is available */
    PGCLONE_JOB_PENDING,      /* Job queued, not started */
    PGCLONE_JOB_RUNNING,      /* Job in progress */
    PGCLONE_JOB_COMPLETED,    /* Job finished successfully */
    PGCLONE_JOB_FAILED,       /* Job finished with error */
    PGCLONE_JOB_CANCELLED     /* Job was cancelled */
} PgcloneJobStatus;

/* Clone operation type */
typedef enum PgcloneOpType
{
    PGCLONE_OP_TABLE = 1,
    PGCLONE_OP_SCHEMA,
    PGCLONE_OP_DATABASE
} PgcloneOpType;

/* Conflict resolution strategy */
typedef enum PgcloneConflictStrategy
{
    PGCLONE_CONFLICT_ERROR = 0,   /* Error if target exists (default) */
    PGCLONE_CONFLICT_SKIP,        /* Skip if target exists */
    PGCLONE_CONFLICT_REPLACE,     /* DROP and re-create target */
    PGCLONE_CONFLICT_RENAME       /* Rename target with _old suffix */
} PgcloneConflictStrategy;

/* Single job entry in shared memory */
typedef struct PgcloneJob
{
    /* Job identification */
    int             job_id;
    pid_t           worker_pid;
    PgcloneJobStatus status;
    PgcloneOpType  op_type;

    /* Connection info */
    char            source_conninfo[1024];
    char            schema_name[NAMEDATALEN];
    char            table_name[NAMEDATALEN];
    char            target_name[NAMEDATALEN];

    /* Options */
    bool            include_data;
    bool            include_indexes;
    bool            include_constraints;
    bool            include_triggers;
    PgcloneConflictStrategy conflict_strategy;
    int             parallel_workers;   /* 0 = sequential */

    /* Progress tracking */
    int64           total_tables;
    int64           completed_tables;
    int64           total_rows;
    int64           copied_rows;
    int64           total_bytes;
    int64           copied_bytes;
    char            current_table[NAMEDATALEN];
    char            current_phase[64];  /* "copying data", "creating indexes", etc */

    /* Resume support */
    bool            resumable;
    char            resume_checkpoint[NAMEDATALEN]; /* last completed table */

    /* Timing */
    TimestampTz     start_time;
    TimestampTz     end_time;

    /* Error info */
    char            error_message[256];

    /* Database for loopback connection */
    Oid             database_oid;
    char            database_name[NAMEDATALEN];
    char            username[NAMEDATALEN];
} PgcloneJob;

/* Single task in the worker pool queue */
typedef struct PgclonePoolTask
{
    char            table_name[NAMEDATALEN];
    int             status;             /* 0=pending, 1=in_progress, 2=done, 3=failed */
    int             claimed_by_job_id;  /* job_id of the worker that claimed this task */
} PgclonePoolTask;

/* Worker pool queue in shared memory */
typedef struct PgclonePoolQueue
{
    bool            active;             /* true when a pool operation is running */
    int             parent_job_id;
    int             num_tasks;
    int             next_task_idx;      /* next unclaimed task index */
    int             completed_count;
    int             failed_count;

    /* Shared parameters for all pool workers */
    char            source_conninfo[1024];
    char            schema_name[NAMEDATALEN];
    bool            include_data;
    bool            include_indexes;
    bool            include_constraints;
    bool            include_triggers;
    PgcloneConflictStrategy conflict_strategy;

    /* Database context for workers */
    Oid             database_oid;
    char            database_name[NAMEDATALEN];
    char            username[NAMEDATALEN];

    PgclonePoolTask tasks[PGCLONE_MAX_POOL_TASKS];
} PgclonePoolQueue;

/* Shared memory state */
typedef struct PgcloneSharedState
{
    LWLock         *lock;
    int             next_job_id;
    PgcloneJob     jobs[PGCLONE_MAX_JOBS];
    PgclonePoolQueue pool;
} PgcloneSharedState;

/* Global pointer to shared state */
extern PgcloneSharedState *pgclone_state;

/* Function declarations */
#if defined(__GNUC__) || defined(__clang__)
extern __attribute__((visibility("default"))) void pgclone_bgw_main(Datum main_arg);
extern __attribute__((visibility("default"))) void pgclone_pool_worker_main(Datum main_arg);
#else
extern PGDLLEXPORT void pgclone_bgw_main(Datum main_arg);
extern PGDLLEXPORT void pgclone_pool_worker_main(Datum main_arg);
#endif
extern Size pgclone_shmem_size(void);
extern void pgclone_shmem_init(void);

/* Job lookup helpers (used by both main file and bgw) */
static inline PgcloneJob *
find_job(int job_id)
{
    int i;
    for (i = 0; i < PGCLONE_MAX_JOBS; i++)
    {
        if (pgclone_state->jobs[i].job_id == job_id)
            return &pgclone_state->jobs[i];
    }
    return NULL;
}

static inline PgcloneJob *
find_free_slot(void)
{
    int i;
    for (i = 0; i < PGCLONE_MAX_JOBS; i++)
    {
        if (pgclone_state->jobs[i].status == PGCLONE_JOB_FREE)
            return &pgclone_state->jobs[i];
    }
    return NULL;
}

#endif /* PGCLONE_BGW_H */
