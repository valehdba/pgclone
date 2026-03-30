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
} PgcloneJob;

/* Shared memory state */
typedef struct PgcloneSharedState
{
    LWLock         *lock;
    int             next_job_id;
    PgcloneJob     jobs[PGCLONE_MAX_JOBS];
} PgcloneSharedState;

/* Global pointer to shared state */
extern PgcloneSharedState *pgclone_state;

/* Function declarations */
extern void pgclone_bgw_main(Datum main_arg);
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
