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

#endif /* PGCLONE_COMPAT_H */
