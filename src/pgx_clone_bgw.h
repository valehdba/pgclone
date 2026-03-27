/*
 * pgx_clone_bgw.h - Background worker definitions for pgx_clone
 *
 * Defines the shared memory structures for job tracking,
 * progress reporting, and parallel clone coordination.
 */

#ifndef PGX_CLONE_BGW_H
#define PGX_CLONE_BGW_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

/* Maximum concurrent clone jobs */
#define PGX_CLONE_MAX_JOBS      16

/* Job status enum */
typedef enum PgxCloneJobStatus
{
    PGX_CLONE_JOB_FREE = 0,     /* Slot is available */
    PGX_CLONE_JOB_PENDING,      /* Job queued, not started */
    PGX_CLONE_JOB_RUNNING,      /* Job in progress */
    PGX_CLONE_JOB_COMPLETED,    /* Job finished successfully */
    PGX_CLONE_JOB_FAILED,       /* Job finished with error */
    PGX_CLONE_JOB_CANCELLED     /* Job was cancelled */
} PgxCloneJobStatus;

/* Clone operation type */
typedef enum PgxCloneOpType
{
    PGX_CLONE_OP_TABLE = 1,
    PGX_CLONE_OP_SCHEMA,
    PGX_CLONE_OP_DATABASE
} PgxCloneOpType;

/* Conflict resolution strategy */
typedef enum PgxCloneConflictStrategy
{
    PGX_CLONE_CONFLICT_ERROR = 0,   /* Error if target exists (default) */
    PGX_CLONE_CONFLICT_SKIP,        /* Skip if target exists */
    PGX_CLONE_CONFLICT_REPLACE,     /* DROP and re-create target */
    PGX_CLONE_CONFLICT_RENAME       /* Rename target with _old suffix */
} PgxCloneConflictStrategy;

/* Single job entry in shared memory */
typedef struct PgxCloneJob
{
    /* Job identification */
    int             job_id;
    pid_t           worker_pid;
    PgxCloneJobStatus status;
    PgxCloneOpType  op_type;

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
    PgxCloneConflictStrategy conflict_strategy;
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
} PgxCloneJob;

/* Shared memory state */
typedef struct PgxCloneSharedState
{
    LWLock         *lock;
    int             next_job_id;
    PgxCloneJob     jobs[PGX_CLONE_MAX_JOBS];
} PgxCloneSharedState;

/* Global pointer to shared state */
extern PgxCloneSharedState *pgx_clone_state;

/* Function declarations */
extern void pgx_clone_bgw_main(Datum main_arg);
extern Size pgx_clone_shmem_size(void);
extern void pgx_clone_shmem_init(void);

/* Job lookup helpers (used by both main file and bgw) */
static inline PgxCloneJob *
find_job(int job_id)
{
    int i;
    for (i = 0; i < PGX_CLONE_MAX_JOBS; i++)
    {
        if (pgx_clone_state->jobs[i].job_id == job_id)
            return &pgx_clone_state->jobs[i];
    }
    return NULL;
}

static inline PgxCloneJob *
find_free_slot(void)
{
    int i;
    for (i = 0; i < PGX_CLONE_MAX_JOBS; i++)
    {
        if (pgx_clone_state->jobs[i].status == PGX_CLONE_JOB_FREE)
            return &pgx_clone_state->jobs[i];
    }
    return NULL;
}

#endif /* PGX_CLONE_BGW_H */
