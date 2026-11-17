/*
 * pgclone_compat.h - central registry of PostgreSQL version gates.
 *
 * pgclone supports PG14-18 and picks up new versions as they ship.
 * Rather than sprinkle #if PG_VERSION_NUM checks across every TU,
 * every version-dependent choice lives here as a named macro or
 * inline compatibility shim. Adding support for a new PG major
 * becomes a review of this single file.
 *
 * Rule for adding a new gate: give it a semantic name that says
 * WHY it exists, not just the numeric version it fires on
 * (PGCLONE_HAS_TRANSACTION_TIMEOUT, not PGCLONE_PG17_PLUS).
 */

#ifndef PGCLONE_COMPAT_H
#define PGCLONE_COMPAT_H

#include "postgres.h"

/*
 * PG 15 moved shared-memory requests out of _PG_init() and into a
 * new `shmem_request_hook`. Callers wanting the pre-PG15 direct
 * path key off !PGCLONE_HAS_SHMEM_REQUEST_HOOK.
 */
#if PG_VERSION_NUM >= 150000
#define PGCLONE_HAS_SHMEM_REQUEST_HOOK 1
#else
#define PGCLONE_HAS_SHMEM_REQUEST_HOOK 0
#endif

/*
 * PG 17 introduced SignalHandlerForShutdownRequest as the
 * canonical SIGTERM handler for bgworkers, replacing the older
 * ProcessInterrupts-based `die`. The include header also moved
 * (postmaster/interrupt.h vs tcop/tcopprot.h). pgclone_shutdown_handler
 * resolves to whichever is current, so pqsignal(SIGTERM, ...) sites
 * do not need to gate.
 */
#if PG_VERSION_NUM >= 170000
#include "postmaster/interrupt.h"
#define pgclone_shutdown_handler SignalHandlerForShutdownRequest
#else
#include "tcop/tcopprot.h"
#define pgclone_shutdown_handler die
#endif

/*
 * PGCLONE_CHECK_SHUTDOWN: interrupt check for long loops in a bgworker.
 * Before PG 17 the SIGTERM handler is die(), which sets ProcDiePending
 * and is serviced by CHECK_FOR_INTERRUPTS(). From PG 17 it is
 * SignalHandlerForShutdownRequest(), which only sets
 * ShutdownRequestPending, so that flag must be tested explicitly or
 * SIGTERM / server shutdown is ignored until the loop ends.
 */
#if PG_VERSION_NUM >= 170000
#define PGCLONE_CHECK_SHUTDOWN() \
    do { \
        CHECK_FOR_INTERRUPTS(); \
        if (ShutdownRequestPending) \
            ereport(FATAL, \
                    (errcode(ERRCODE_ADMIN_SHUTDOWN), \
                     errmsg("terminating pgclone worker due to administrator command"))); \
    } while (0)
#else
#define PGCLONE_CHECK_SHUTDOWN() CHECK_FOR_INTERRUPTS()
#endif

/*
 * Column suffix for CREATE TABLE reconstruction.
 *
 * pg_attrdef holds both ordinary defaults and the generation
 * expression of a GENERATED ALWAYS column. Treating every adbin as
 * DEFAULT turns generated columns into plain defaults. 's' is STORED
 * (PG 12+). 'v' is VIRTUAL (PG 18+); on older servers that comparison
 * never matches, and the SQL is valid there because it is only a
 * string compare. Identity columns (attidentity <> '') still fall
 * through to DEFAULT, as before.
 *
 * The leading space is significant: this fragment is concatenated
 * after the type (and an optional NOT NULL).
 */
#define PGCLONE_COLUMN_ATTRDEF_SQL \
    "CASE " \
    "WHEN a.attgenerated = 's' THEN " \
    "' GENERATED ALWAYS AS (' || pg_get_expr(d.adbin, d.adrelid) || ') STORED' " \
    "WHEN a.attgenerated = 'v' THEN " \
    "' GENERATED ALWAYS AS (' || pg_get_expr(d.adbin, d.adrelid) || ') VIRTUAL' " \
    "WHEN d.adbin IS NOT NULL THEN " \
    "' DEFAULT ' || pg_get_expr(d.adbin, d.adrelid) " \
    "ELSE '' END"

/*
 * ORDER BY expression for a pg_tables row (schemaname / tablename in
 * scope): how many inheritance / partition ancestors the table has.
 * Sorting by it creates partitioned parents before their partitions
 * (and sub-partitioned partitions before their own partitions).
 */
#define PGCLONE_TABLE_DEPTH_SQL \
    "(WITH RECURSIVE anc(oid, d) AS (" \
    "SELECT (pg_catalog.quote_ident(schemaname) || '.' || pg_catalog.quote_ident(tablename))::regclass::oid, 0 " \
    "UNION ALL " \
    "SELECT i.inhparent, anc.d + 1 FROM pg_catalog.pg_inherits i " \
    "JOIN anc ON i.inhrelid = anc.oid) " \
    "SELECT max(d) FROM anc)"

#endif /* PGCLONE_COMPAT_H */
