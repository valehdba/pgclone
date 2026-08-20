/*
 * pgclone_snapshot.c - shared source-side snapshot helpers.
 *
 * See pgclone_snapshot.h for the API contract. Previously these
 * helpers were duplicated in pgclone.c (sync path) and pgclone_bgw.c
 * (bgw path). Consolidating here means transaction-timeout,
 * keepalive, and snapshot-import bug fixes now live in one place
 * (previously issues #5 and #9 had to be patched twice).
 */

#include "postgres.h"
#include "utils/builtins.h"
#include "lib/stringinfo.h"
#include "libpq-fe.h"

#include "pgclone_snapshot.h"

bool
pgclone_snap_begin_repeatable_read(PGconn *conn)
{
    PGresult *res;

    if (PQtransactionStatus(conn) == PQTRANS_INTRANS)
        return true;

    res = PQexec(conn, "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        PQclear(res);
        return false;
    }
    PQclear(res);

    /* Defeat server-side timeouts for the keeper's idle window.
     * SET LOCAL reverts at COMMIT and the GUCs are PGC_USERSET, so
     * no special privilege is required. Failures here are non-fatal
     * — TCP keepalives still protect us against the firewall path.
     * transaction_timeout is PG17+, so gate on the SOURCE server
     * version (PQserverVersion) rather than the backend's
     * PG_VERSION_NUM: the source may run a different major version. */
    {
        StringInfoData setcmd;

        initStringInfo(&setcmd);
        appendStringInfoString(&setcmd,
            "SET LOCAL idle_in_transaction_session_timeout = 0; "
            "SET LOCAL statement_timeout = 0");

        if (PQserverVersion(conn) >= 170000)
            appendStringInfoString(&setcmd,
                "; SET LOCAL transaction_timeout = 0");

        res = PQexec(conn, setcmd.data);
        if (PQresultStatus(res) != PGRES_COMMAND_OK)
            elog(DEBUG1,
                 "pgclone: could not disable source-side timeouts: %s",
                 PQerrorMessage(conn));
        PQclear(res);
        pfree(setcmd.data);
    }

    return true;
}

void
pgclone_snap_commit_source(PGconn *conn)
{
    PGresult *res;

    if (PQtransactionStatus(conn) != PQTRANS_INTRANS)
        return;

    res = PQexec(conn, "COMMIT");
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
        elog(DEBUG1, "pgclone: COMMIT on source returned: %s",
             PQerrorMessage(conn));
    PQclear(res);
}

bool
pgclone_snap_export(PGconn *conn, char *out_id, size_t out_id_len)
{
    PGresult   *res;
    const char *snap;
    size_t      slen;

    res = PQexec(conn, "SELECT pg_catalog.pg_export_snapshot()");
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1)
    {
        PQclear(res);
        return false;
    }

    snap = PQgetvalue(res, 0, 0);
    slen = strlen(snap);
    if (slen >= out_id_len)
    {
        PQclear(res);
        return false;
    }
    memcpy(out_id, snap, slen);
    out_id[slen] = '\0';
    PQclear(res);
    return true;
}

bool
pgclone_snap_import(PGconn *conn, const char *snapshot_id)
{
    PGresult       *res;
    StringInfoData  cmd;

    if (!pgclone_snap_begin_repeatable_read(conn))
        return false;

    initStringInfo(&cmd);
    appendStringInfo(&cmd, "SET TRANSACTION SNAPSHOT %s",
                     quote_literal_cstr(snapshot_id));
    res = PQexec(conn, cmd.data);
    pfree(cmd.data);

    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        PQclear(res);
        return false;
    }
    PQclear(res);
    return true;
}

bool
pgclone_snap_keeper_ping(PGconn *conn)
{
    PGresult *res;

    if (conn == NULL)
        return true;
    if (PQtransactionStatus(conn) != PQTRANS_INTRANS)
        return true;
    if (PQstatus(conn) != CONNECTION_OK)
        return false;

    res = PQexec(conn, "SELECT 1");
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PQclear(res);
        return false;
    }
    PQclear(res);
    return true;
}
