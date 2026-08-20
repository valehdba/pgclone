/*
 * pgclone_snapshot.h - source-side snapshot/keeper helpers.
 *
 * Wraps the libpq operations used to open a REPEATABLE READ READ ONLY
 * transaction on the source connection, export/import a snapshot for
 * cross-connection consistency, and probe that the keeper transaction
 * is still alive. Historically these helpers were duplicated in
 * pgclone.c (sync path, ereport-on-error) and pgclone_bgw.c
 * (background-worker path, WARNING + return-bool). Both callers now
 * share one implementation here.
 *
 * All helpers return bool; on failure PQerrorMessage(conn) holds the
 * libpq error text. Sync callers wrap failures in ereport(ERROR);
 * bgw callers log WARNING and abort the job. Neither ereport nor
 * elog(ERROR) is issued from inside these helpers.
 */

#ifndef PGCLONE_SNAPSHOT_H
#define PGCLONE_SNAPSHOT_H

#include "postgres.h"
#include "libpq-fe.h"

/* BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY on conn, then
 * SET LOCAL idle_in_transaction_session_timeout / statement_timeout
 * (and, on PG17+ sources, transaction_timeout) to 0 so the keeper's
 * idle window is not reaped mid-clone. No-op if conn is already in
 * a transaction. Returns true on the BEGIN succeeding; SET LOCAL
 * failures are logged at DEBUG1 and do not affect the return. */
extern bool pgclone_snap_begin_repeatable_read(PGconn *conn);

/* COMMIT the current transaction on conn. No-op if no transaction
 * is open. Errors are logged at DEBUG1 — the connection is typically
 * about to be closed anyway. */
extern void pgclone_snap_commit_source(PGconn *conn);

/* SELECT pg_export_snapshot() on a conn that must already be inside
 * a REPEATABLE READ READ ONLY transaction; writes the id into out_id.
 * Returns false if the query fails or if the id would not fit. */
extern bool pgclone_snap_export(PGconn *conn, char *out_id, size_t out_id_len);

/* pgclone_snap_begin_repeatable_read + SET TRANSACTION SNAPSHOT. The
 * exporting keeper transaction must still be alive. Returns false if
 * either step fails. Callers should inspect PQerrorMessage(conn) for
 * "invalid snapshot identifier" to detect a reaped keeper. */
extern bool pgclone_snap_import(PGconn *conn, const char *snapshot_id);

/* Cheap round-trip to detect a silently-dropped keeper before the
 * next importer binds to a snapshot the server has already reaped.
 * Returns true when the keeper is healthy (or when there is no
 * open transaction to protect). */
extern bool pgclone_snap_keeper_ping(PGconn *conn);

#endif /* PGCLONE_SNAPSHOT_H */
