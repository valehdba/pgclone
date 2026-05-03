/*
 * pgclone - PostgreSQL extension for cloning databases, schemas, tables,
 *            and functions between PostgreSQL hosts.
 *
 * Copyright (c) 2026, Valeh Agayev pgclone contributors
 * Licensed under PostgreSQL License
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "executor/spi.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "catalog/pg_type.h"
#include "utils/syscache.h"
#include "utils/guc.h"
#include "commands/dbcommands.h"
#include "utils/jsonb.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

#include "pgclone_bgw.h"

PG_MODULE_MAGIC;

/* ---------------------------------------------------------------
 * Clone options struct — controls what gets cloned beyond the
 * table structure and data.
 * --------------------------------------------------------------- */
#define PGCLONE_MAX_COLUMNS   64
#define PGCLONE_MAX_WHERE     2048
#define PGCLONE_MAX_MASKS     64

/* Mask strategy types */
typedef enum PgcloneMaskType
{
    PGCLONE_MASK_NONE = 0,
    PGCLONE_MASK_EMAIL,         /* a***@domain.com */
    PGCLONE_MASK_NAME,          /* XXXX */
    PGCLONE_MASK_PHONE,         /* ***-***-1234 */
    PGCLONE_MASK_PARTIAL,       /* Jo***on — keep first/last N chars */
    PGCLONE_MASK_HASH,          /* SHA256 — deterministic for referential integrity */
    PGCLONE_MASK_NULL,          /* replace with NULL */
    PGCLONE_MASK_RANDOM_INT,    /* random integer in [min, max] */
    PGCLONE_MASK_CONSTANT       /* fixed replacement value */
} PgcloneMaskType;

/* Per-column masking rule */
typedef struct MaskRule
{
    char              column[NAMEDATALEN];
    PgcloneMaskType   type;
    int               partial_prefix;  /* chars to keep at start (PARTIAL) */
    int               partial_suffix;  /* chars to keep at end (PARTIAL) */
    int               range_min;       /* RANDOM_INT min */
    int               range_max;       /* RANDOM_INT max */
    char              constant_val[256]; /* CONSTANT replacement */
} MaskRule;

typedef struct CloneOptions
{
    bool include_indexes;
    bool include_constraints;
    bool include_triggers;
    bool include_matviews;       /* clone materialized views */

    /* Selective column cloning: if num_columns > 0, only these columns */
    int  num_columns;
    char columns[PGCLONE_MAX_COLUMNS][NAMEDATALEN];

    /* Data filtering: WHERE clause applied during COPY */
    char where_clause[PGCLONE_MAX_WHERE];

    /* Parallel cloning: number of workers (0 = sequential) */
    int  parallel_workers;

    /* Data masking rules: column-level anonymization */
    int  num_masks;
    MaskRule masks[PGCLONE_MAX_MASKS];
} CloneOptions;

/* Default: everything enabled, no column/where/mask filter */
static CloneOptions
pgclone_default_options(void)
{
    CloneOptions opts;
    memset(&opts, 0, sizeof(CloneOptions));
    opts.include_indexes     = true;
    opts.include_constraints = true;
    opts.include_triggers    = true;
    opts.include_matviews    = true;
    opts.num_columns         = 0;
    opts.where_clause[0]     = '\0';
    opts.parallel_workers    = 0;
    opts.num_masks           = 0;
    return opts;
}

/* ---------------------------------------------------------------
 * Parse a JSON options string like:
 *   {"indexes": false, "constraints": true, "triggers": false,
 *    "columns": ["id", "name", "email"],
 *    "where": "status = 'active' AND created_at > '2024-01-01'"}
 *
 * Missing keys keep defaults. Simple parser — no external deps.
 * --------------------------------------------------------------- */
static CloneOptions
pgclone_parse_options(const char *json_str)
{
    CloneOptions opts = pgclone_default_options();
    const char *p;

    if (json_str == NULL || json_str[0] == '\0')
        return opts;

    /* Boolean options */
    if (strstr(json_str, "\"indexes\": false") != NULL ||
        strstr(json_str, "\"indexes\":false") != NULL)
        opts.include_indexes = false;

    if (strstr(json_str, "\"constraints\": false") != NULL ||
        strstr(json_str, "\"constraints\":false") != NULL)
        opts.include_constraints = false;

    if (strstr(json_str, "\"triggers\": false") != NULL ||
        strstr(json_str, "\"triggers\":false") != NULL)
        opts.include_triggers = false;

    if (strstr(json_str, "\"matviews\": false") != NULL ||
        strstr(json_str, "\"matviews\":false") != NULL)
        opts.include_matviews = false;

    /* Parse "parallel": N */
    {
        const char *pp = strstr(json_str, "\"parallel\"");
        if (pp != NULL)
        {
            const char *colon = strchr(pp, ':');
            if (colon != NULL)
            {
                int val = atoi(colon + 1);
                if (val > 0 && val <= PGCLONE_MAX_JOBS)
                    opts.parallel_workers = val;
            }
        }
    }

    /* Parse "columns": ["col1", "col2", ...] */
    p = strstr(json_str, "\"columns\"");
    if (p != NULL)
    {
        const char *bracket = strchr(p, '[');
        if (bracket != NULL)
        {
            const char *end_bracket = strchr(bracket, ']');
            if (end_bracket != NULL)
            {
                const char *cur = bracket + 1;
                opts.num_columns = 0;

                while (cur < end_bracket && opts.num_columns < PGCLONE_MAX_COLUMNS)
                {
                    const char *quote_start, *quote_end;

                    /* Find opening quote */
                    quote_start = strchr(cur, '"');
                    if (!quote_start || quote_start >= end_bracket)
                        break;
                    quote_start++;

                    /* Find closing quote */
                    quote_end = strchr(quote_start, '"');
                    if (!quote_end || quote_end >= end_bracket)
                        break;

                    {
                        int len = quote_end - quote_start;
                        if (len > 0 && len < NAMEDATALEN)
                        {
                            memcpy(opts.columns[opts.num_columns], quote_start, len);
                            opts.columns[opts.num_columns][len] = '\0';
                            opts.num_columns++;
                        }
                    }

                    cur = quote_end + 1;
                }
            }
        }
    }

    /* Parse "where": "condition..." */
    p = strstr(json_str, "\"where\"");
    if (p == NULL)
        p = strstr(json_str, "\"filter\"");  /* accept "filter" as alias */

    if (p != NULL)
    {
        const char *colon = strchr(p, ':');
        if (colon != NULL)
        {
            const char *quote_start = strchr(colon, '"');
            if (quote_start != NULL)
            {
                const char *quote_end;
                quote_start++;

                /* Find end quote — handle escaped quotes */
                quote_end = quote_start;
                while (*quote_end != '\0')
                {
                    if (*quote_end == '\\' && *(quote_end + 1) == '"')
                    {
                        quote_end += 2;
                        continue;
                    }
                    if (*quote_end == '"')
                        break;
                    quote_end++;
                }

                {
                    int len = quote_end - quote_start;
                    if (len > 0 && len < PGCLONE_MAX_WHERE)
                    {
                        memcpy(opts.where_clause, quote_start, len);
                        opts.where_clause[len] = '\0';
                    }
                }
            }
        }
    }

    /* Parse "mask": {"col": "type", "col": {"type":"partial", ...}} */
    p = strstr(json_str, "\"mask\"");
    if (p != NULL)
    {
        const char *brace = strchr(p, '{');
        if (brace != NULL)
        {
            /* Skip to the inner object brace (the one after "mask":) */
            const char *cur = brace + 1;
            int brace_depth = 1;
            const char *mask_end = cur;

            /* Find matching closing brace */
            while (*mask_end && brace_depth > 0)
            {
                if (*mask_end == '{') brace_depth++;
                else if (*mask_end == '}') brace_depth--;
                if (brace_depth > 0) mask_end++;
            }

            /* Parse key-value pairs inside the mask object */
            while (cur < mask_end && opts.num_masks < PGCLONE_MAX_MASKS)
            {
                const char *key_start, *key_end;
                const char *val_start;
                MaskRule *rule = &opts.masks[opts.num_masks];

                memset(rule, 0, sizeof(MaskRule));
                rule->partial_prefix = 1;
                rule->partial_suffix = 1;

                /* Find key (column name) */
                key_start = strchr(cur, '"');
                if (!key_start || key_start >= mask_end) break;
                key_start++;
                key_end = strchr(key_start, '"');
                if (!key_end || key_end >= mask_end) break;

                {
                    int klen = key_end - key_start;
                    if (klen > 0 && klen < NAMEDATALEN)
                    {
                        memcpy(rule->column, key_start, klen);
                        rule->column[klen] = '\0';
                    }
                }

                /* Skip to colon */
                cur = key_end + 1;
                while (cur < mask_end && *cur != ':') cur++;
                if (cur >= mask_end) break;
                cur++; /* skip ':' */

                /* Skip whitespace */
                while (cur < mask_end && (*cur == ' ' || *cur == '\t' || *cur == '\n')) cur++;

                if (*cur == '"')
                {
                    /* Simple string value: "email", "name", "hash", etc. */
                    val_start = cur + 1;
                    {
                        const char *val_end = strchr(val_start, '"');
                        if (val_end && val_end < mask_end)
                        {
                            char val_buf[64];
                            int vlen = val_end - val_start;
                            if (vlen > 0 && vlen < (int)sizeof(val_buf))
                            {
                                memcpy(val_buf, val_start, vlen);
                                val_buf[vlen] = '\0';

                                if (strcmp(val_buf, "email") == 0)
                                    rule->type = PGCLONE_MASK_EMAIL;
                                else if (strcmp(val_buf, "name") == 0)
                                    rule->type = PGCLONE_MASK_NAME;
                                else if (strcmp(val_buf, "phone") == 0)
                                    rule->type = PGCLONE_MASK_PHONE;
                                else if (strcmp(val_buf, "partial") == 0)
                                    rule->type = PGCLONE_MASK_PARTIAL;
                                else if (strcmp(val_buf, "hash") == 0)
                                    rule->type = PGCLONE_MASK_HASH;
                                else if (strcmp(val_buf, "null") == 0)
                                    rule->type = PGCLONE_MASK_NULL;
                                else if (strcmp(val_buf, "random_int") == 0)
                                    rule->type = PGCLONE_MASK_RANDOM_INT;
                                else if (strcmp(val_buf, "constant") == 0)
                                {
                                    rule->type = PGCLONE_MASK_CONSTANT;
                                    /* Simple string "constant" has no value — default to empty.
                                     * Use object form for custom value: {"type":"constant","value":"X"} */
                                    if (rule->constant_val[0] == '\0')
                                        strlcpy(rule->constant_val, "REDACTED",
                                                sizeof(rule->constant_val));
                                }
                                else
                                    ereport(WARNING,
                                            (errmsg("pgclone: unknown mask type \"%s\" for column \"%s\", skipping",
                                                    val_buf, rule->column)));
                            }
                            cur = val_end + 1;
                        }
                    }
                }
                else if (*cur == '{')
                {
                    /* Object value: {"type":"partial", "prefix":2, "suffix":3} */
                    const char *obj_end = strchr(cur, '}');
                    if (obj_end && obj_end <= mask_end)
                    {
                        const char *type_p = strstr(cur, "\"type\"");
                        if (type_p && type_p < obj_end)
                        {
                            const char *tq = strchr(type_p + 6, '"');
                            if (tq && tq < obj_end)
                            {
                                const char *tq2 = strchr(tq + 1, '"');
                                if (tq2 && tq2 < obj_end)
                                {
                                    char tbuf[64];
                                    int tlen = tq2 - tq - 1;
                                    if (tlen > 0 && tlen < (int)sizeof(tbuf))
                                    {
                                        memcpy(tbuf, tq + 1, tlen);
                                        tbuf[tlen] = '\0';

                                        if (strcmp(tbuf, "partial") == 0)
                                            rule->type = PGCLONE_MASK_PARTIAL;
                                        else if (strcmp(tbuf, "random_int") == 0)
                                            rule->type = PGCLONE_MASK_RANDOM_INT;
                                        else if (strcmp(tbuf, "constant") == 0)
                                            rule->type = PGCLONE_MASK_CONSTANT;
                                        else if (strcmp(tbuf, "email") == 0)
                                            rule->type = PGCLONE_MASK_EMAIL;
                                        else if (strcmp(tbuf, "name") == 0)
                                            rule->type = PGCLONE_MASK_NAME;
                                        else if (strcmp(tbuf, "phone") == 0)
                                            rule->type = PGCLONE_MASK_PHONE;
                                        else if (strcmp(tbuf, "hash") == 0)
                                            rule->type = PGCLONE_MASK_HASH;
                                        else if (strcmp(tbuf, "null") == 0)
                                            rule->type = PGCLONE_MASK_NULL;
                                    }
                                }
                            }
                        }

                        /* Parse numeric params: prefix, suffix, min, max */
                        {
                            const char *pp;
                            pp = strstr(cur, "\"prefix\"");
                            if (pp && pp < obj_end)
                            {
                                const char *c = strchr(pp + 8, ':');
                                if (c && c < obj_end)
                                    rule->partial_prefix = atoi(c + 1);
                            }
                            pp = strstr(cur, "\"suffix\"");
                            if (pp && pp < obj_end)
                            {
                                const char *c = strchr(pp + 8, ':');
                                if (c && c < obj_end)
                                    rule->partial_suffix = atoi(c + 1);
                            }
                            pp = strstr(cur, "\"min\"");
                            if (pp && pp < obj_end)
                            {
                                const char *c = strchr(pp + 5, ':');
                                if (c && c < obj_end)
                                    rule->range_min = atoi(c + 1);
                            }
                            pp = strstr(cur, "\"max\"");
                            if (pp && pp < obj_end)
                            {
                                const char *c = strchr(pp + 5, ':');
                                if (c && c < obj_end)
                                    rule->range_max = atoi(c + 1);
                            }

                            /* Parse "value": "..." for constant type */
                            pp = strstr(cur, "\"value\"");
                            if (pp && pp < obj_end)
                            {
                                const char *vq = strchr(pp + 7, '"');
                                if (vq && vq < obj_end)
                                {
                                    const char *vq2 = strchr(vq + 1, '"');
                                    if (vq2 && vq2 <= obj_end)
                                    {
                                        int vlen = vq2 - vq - 1;
                                        if (vlen > 0 && vlen < (int)sizeof(rule->constant_val))
                                        {
                                            memcpy(rule->constant_val, vq + 1, vlen);
                                            rule->constant_val[vlen] = '\0';
                                        }
                                    }
                                }
                            }
                        }

                        cur = obj_end + 1;
                    }
                }

                if (rule->type != PGCLONE_MASK_NONE)
                    opts.num_masks++;

                /* Advance past comma if present */
                while (cur < mask_end && (*cur == ',' || *cur == ' ' || *cur == '\n' || *cur == '\t'))
                    cur++;
            }
        }
    }

    return opts;
}

/* ---------------------------------------------------------------
 * Build a SQL expression that applies a masking strategy to a column.
 *
 * The expression is pure SQL executed server-side on the source,
 * so masking happens before data enters the COPY stream — no
 * row-by-row processing overhead.
 *
 * All expressions use pgcrypto-free SQL: md5() is built-in,
 * string functions are available on all PG 14+.
 * --------------------------------------------------------------- */
static void
pgclone_build_mask_expr(StringInfo buf, const char *col_ident,
                        const MaskRule *rule)
{
    switch (rule->type)
    {
        case PGCLONE_MASK_EMAIL:
            /*
             * Preserves domain, masks local part:
             *   "alice@example.com" -> "a***@example.com"
             * NULL-safe via COALESCE.
             */
            appendStringInfo(buf,
                "CASE WHEN %s IS NULL THEN NULL "
                "WHEN position('@' in %s::text) > 0 THEN "
                "  left(%s::text, 1) || '***@' || split_part(%s::text, '@', 2) "
                "ELSE '***' END",
                col_ident, col_ident, col_ident, col_ident);
            break;

        case PGCLONE_MASK_NAME:
            /* Replace with fixed string, preserving NULL */
            appendStringInfo(buf,
                "CASE WHEN %s IS NULL THEN NULL ELSE 'XXXX' END",
                col_ident);
            break;

        case PGCLONE_MASK_PHONE:
            /*
             * Keep last 4 characters, mask rest:
             *   "+1-555-123-4567" -> "***-4567"
             */
            appendStringInfo(buf,
                "CASE WHEN %s IS NULL THEN NULL "
                "WHEN length(%s::text) > 4 THEN '***-' || right(%s::text, 4) "
                "ELSE '****' END",
                col_ident, col_ident, col_ident);
            break;

        case PGCLONE_MASK_PARTIAL:
            /*
             * Keep first N and last M chars, mask middle:
             *   prefix=2, suffix=3: "Johnson" -> "Jo***son"
             */
            appendStringInfo(buf,
                "CASE WHEN %s IS NULL THEN NULL "
                "WHEN length(%s::text) <= %d THEN repeat('*', length(%s::text)) "
                "ELSE left(%s::text, %d) || '***' || right(%s::text, %d) END",
                col_ident,
                col_ident, rule->partial_prefix + rule->partial_suffix,
                col_ident,
                col_ident, rule->partial_prefix,
                col_ident, rule->partial_suffix);
            break;

        case PGCLONE_MASK_HASH:
            /*
             * Deterministic MD5 hash — same input always produces same output.
             * Useful for columns that need referential integrity across tables
             * (e.g., hash(email) in table A matches hash(email) in table B).
             */
            appendStringInfo(buf,
                "CASE WHEN %s IS NULL THEN NULL "
                "ELSE md5(%s::text) END",
                col_ident, col_ident);
            break;

        case PGCLONE_MASK_NULL:
            appendStringInfo(buf, "NULL");
            break;

        case PGCLONE_MASK_RANDOM_INT:
            {
                int rmin = rule->range_min;
                int rmax = rule->range_max;

                /* Default range if not specified (both zero from memset) */
                if (rmin == 0 && rmax == 0)
                    rmax = 99999;

                /* Swap if inverted */
                if (rmin > rmax)
                {
                    int tmp = rmin;
                    rmin = rmax;
                    rmax = tmp;
                }

                appendStringInfo(buf,
                    "floor(random() * (%d - %d + 1) + %d)::integer",
                    rmax, rmin, rmin);
            }
            break;

        case PGCLONE_MASK_CONSTANT:
            appendStringInfo(buf, "%s",
                             quote_literal_cstr(rule->constant_val));
            break;

        case PGCLONE_MASK_NONE:
            /* Should not reach here, but emit column as-is */
            appendStringInfoString(buf, col_ident);
            break;
    }
}

/* ---------------------------------------------------------------
 * Find a mask rule for a given column name, or NULL if none.
 * --------------------------------------------------------------- */
static const MaskRule *
pgclone_find_mask_rule(const CloneOptions *opts, const char *col_name)
{
    int i;

    if (opts == NULL || opts->num_masks == 0)
        return NULL;

    for (i = 0; i < opts->num_masks; i++)
    {
        if (strcmp(opts->masks[i].column, col_name) == 0)
            return &opts->masks[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------
 * Internal helper: pin the source session to a deterministic search_path.
 *
 * pg_get_triggerdef(), pg_get_expr() (used for column DEFAULTs), and
 * pg_get_indexdef() all call generate_relation_name() internally, which
 * SUPPRESSES the schema prefix when the relation's namespace appears on
 * the current session's search_path.  When source DBs have an application
 * schema on search_path (a common production pattern, e.g. via
 * "ALTER DATABASE ... SET search_path = app, public"), every DDL we
 * extract for that schema comes back with bare relation names.  Replaying
 * those on the target loopback connection — whose search_path does not
 * include that schema — fails with "relation X does not exist".
 *
 * Pinning to pg_catalog forces full schema-qualification of every
 * non-pg_catalog object reference.  Built-in types stay unqualified
 * (so type names in DDL remain readable) because pg_catalog *is* on
 * search_path.  This matches what pg_dump --no-search-path does.
 *
 * Failure here is logged at WARNING and not propagated — the worst case
 * is the original behaviour, not a hard error.
 * --------------------------------------------------------------- */
static void
pgclone_normalize_session(PGconn *conn)
{
    PGresult *res = PQexec(conn, "SET search_path = pg_catalog");
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
        ereport(WARNING,
                (errmsg("pgclone: could not set source search_path: %s",
                        PQerrorMessage(conn))));
    PQclear(res);
}

/* ---------------------------------------------------------------
 * Internal helper: connect to a remote PostgreSQL host
 * --------------------------------------------------------------- */
static PGconn *
pgclone_connect(const char *conninfo)
{
    PGconn *conn;

    conn = PQconnectdb(conninfo);

    if (PQstatus(conn) != CONNECTION_OK)
    {
        char *conn_errmsg = pstrdup(PQerrorMessage(conn));
        PQfinish(conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgclone: could not connect to remote host: %s", conn_errmsg)));
    }

    pgclone_normalize_session(conn);

    return conn;
}

/* ---------------------------------------------------------------
 * Internal helper: execute a query on a remote connection
 * Returns the PGresult (caller must PQclear it)
 * --------------------------------------------------------------- */
static PGresult *
pgclone_exec(PGconn *conn, const char *query)
{
    PGresult *res;

    res = PQexec(conn, query);

    if (PQresultStatus(res) != PGRES_TUPLES_OK &&
        PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        char *exec_errmsg = pstrdup(PQerrorMessage(conn));
        PQclear(res);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: remote query failed: %s", exec_errmsg)));
    }

    return res;
}

/* ---------------------------------------------------------------
 * Internal helper: execute DDL on the local database via SPI
 * --------------------------------------------------------------- */
static void
pgclone_exec_local(const char *query)
{
    int ret;

    ret = SPI_execute(query, false, 0);

    if (ret != SPI_OK_UTILITY && ret != SPI_OK_SELECT &&
        ret != SPI_OK_INSERT && ret != SPI_OK_UPDATE)
    {
        ereport(WARNING,
                (errmsg("pgclone: local execution returned code %d for: %.128s",
                        ret, query)));
    }
}

/* ---------------------------------------------------------------
 * Internal helper: execute DDL on a libpq connection.
 * Used for loopback connection operations.
 * Returns true on success, false on failure (with WARNING).
 * --------------------------------------------------------------- */
static bool
pgclone_exec_conn(PGconn *conn, const char *query)
{
    PGresult *res;

    res = PQexec(conn, query);

    if (PQresultStatus(res) != PGRES_COMMAND_OK &&
        PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        char *conn_errmsg = pstrdup(PQerrorMessage(conn));
        PQclear(res);
        ereport(WARNING,
                (errmsg("pgclone: local exec failed: %s (query: %.128s)",
                        conn_errmsg, query)));
        return false;
    }

    PQclear(res);
    return true;
}

/* ---------------------------------------------------------------
 * Internal helper: append host parameter to conninfo.
 * Prefers Unix domain socket (from unix_socket_directories GUC)
 * over TCP 127.0.0.1. Unix sockets use 'local' pg_hba.conf lines
 * with peer auth, eliminating the need for trust on 127.0.0.1.
 * Falls back to TCP if no socket directory is configured.
 * --------------------------------------------------------------- */
static void
pgclone_append_local_host(StringInfo conninfo)
{
    const char *socket_dir;

    socket_dir = GetConfigOption("unix_socket_directories", false, false);

    if (socket_dir && socket_dir[0])
    {
        /* Take only the first directory if comma-separated list */
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

        appendStringInfo(conninfo, "host=%s", first_dir);
        pfree(first_dir);
    }
    else
    {
        appendStringInfoString(conninfo, "host=127.0.0.1");
    }
}

/* ---------------------------------------------------------------
 * Internal helper: get a libpq connection to the LOCAL database.
 * --------------------------------------------------------------- */
static PGconn *
pgclone_connect_local(void)
{
    PGconn         *conn;
    StringInfoData  conninfo;
    const char     *dbname;
    const char     *port;
    const char     *username;

    dbname = get_database_name(MyDatabaseId);
    port = GetConfigOption("port", false, false);
    username = GetUserNameFromId(GetUserId(), false);

    initStringInfo(&conninfo);
    pgclone_append_local_host(&conninfo);
    appendStringInfo(&conninfo, " dbname=%s port=%s user=%s",
                     quote_literal_cstr(dbname),
                     port ? port : "5432",
                     username);

    conn = PQconnectdb(conninfo.data);
    pfree(conninfo.data);

    if (PQstatus(conn) != CONNECTION_OK)
    {
        char *local_errmsg = pstrdup(PQerrorMessage(conn));
        PQfinish(conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgclone: could not connect to local database: %s",
                        local_errmsg)));
    }

    return conn;
}

/* ---------------------------------------------------------------
 * Validate WHERE clause against SQL injection patterns.
 *
 * Rejects semicolons (statement chaining) and DDL/DML keywords
 * that have no place in a read-only filter expression.
 * This is a defense-in-depth layer — the source connection also
 * runs inside a READ ONLY transaction.
 * --------------------------------------------------------------- */
static void
pgclone_validate_where_clause(const char *where_clause)
{
    /* Forbidden patterns: case-insensitive whole-word matches */
    static const char *forbidden[] = {
        "INSERT", "UPDATE", "DELETE", "DROP", "CREATE", "ALTER",
        "TRUNCATE", "GRANT", "REVOKE", "COPY", "EXECUTE",
        "CALL", "DO", "SET", "RESET", "LOAD",
        NULL
    };
    const char *p;
    int         i;
    size_t      len = strlen(where_clause);
    char       *upper;

    /* Reject semicolons — no statement chaining */
    if (strchr(where_clause, ';') != NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone: WHERE clause must not contain semicolons"),
                 errhint("Remove ';' from the WHERE clause.")));

    /* Build uppercase copy for keyword matching */
    upper = palloc(len + 1);
    for (p = where_clause; *p; p++)
        upper[p - where_clause] = (*p >= 'a' && *p <= 'z')
                                  ? (*p - 32) : *p;
    upper[len] = '\0';

    for (i = 0; forbidden[i] != NULL; i++)
    {
        const char *found = upper;
        size_t      klen = strlen(forbidden[i]);

        while ((found = strstr(found, forbidden[i])) != NULL)
        {
            /* Check word boundaries: must not be part of a larger identifier */
            bool start_ok = (found == upper) ||
                            !((*(found - 1) >= 'A' && *(found - 1) <= 'Z') ||
                              (*(found - 1) >= 'a' && *(found - 1) <= 'z') ||
                              (*(found - 1) >= '0' && *(found - 1) <= '9') ||
                              *(found - 1) == '_');
            bool end_ok   = (found[klen] == '\0') ||
                            !((found[klen] >= 'A' && found[klen] <= 'Z') ||
                              (found[klen] >= 'a' && found[klen] <= 'z') ||
                              (found[klen] >= '0' && found[klen] <= '9') ||
                              found[klen] == '_');

            if (start_ok && end_ok)
            {
                pfree(upper);
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("pgclone: WHERE clause contains forbidden keyword: %s",
                                forbidden[i]),
                         errhint("The WHERE clause must be a read-only filter expression.")));
            }
            found += klen;
        }
    }

    pfree(upper);
}

/* ---------------------------------------------------------------
 * Internal helper: stream data from source to target using COPY.
 *
 * When columns or where_clause are provided, uses
 * COPY (SELECT cols FROM table WHERE filter) TO STDOUT
 * instead of COPY table TO STDOUT.
 *
 * If a WHERE clause is present, the source query runs inside a
 * READ ONLY transaction to prevent any side effects from injected SQL.
 * --------------------------------------------------------------- */
static int64
pgclone_copy_data(PGconn *source_conn, PGconn *local_conn,
                    const char *schema_name, const char *source_table,
                    const char *target_table, const CloneOptions *opts)
{
    PGresult       *res;
    StringInfoData  cmd;
    char           *buf;
    int             ret;
    int64           bytes_transferred = 0;
    int64           row_count = 0;
    bool            use_query_copy;

    /* Determine if we need query-based COPY (for columns/WHERE/masks) */
    use_query_copy = (opts != NULL &&
                      (opts->num_columns > 0 || opts->where_clause[0] != '\0' ||
                       opts->num_masks > 0));

    /* Validate and sandbox WHERE clause if present */
    if (opts != NULL && opts->where_clause[0] != '\0')
    {
        PGresult *txres;

        pgclone_validate_where_clause(opts->where_clause);

        /* Wrap source query in READ ONLY transaction — prevents any
         * side effects even if validation is bypassed */
        txres = PQexec(source_conn, "BEGIN TRANSACTION READ ONLY");
        if (PQresultStatus(txres) != PGRES_COMMAND_OK)
        {
            char *tx_errmsg = pstrdup(PQerrorMessage(source_conn));
            PQclear(txres);
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("pgclone: could not start read-only transaction on source: %s",
                            tx_errmsg)));
        }
        PQclear(txres);
    }

    /* Start COPY OUT on source */
    initStringInfo(&cmd);

    if (use_query_copy)
    {
        /* COPY (SELECT columns FROM table WHERE filter) TO STDOUT */
        StringInfoData select_cmd;
        initStringInfo(&select_cmd);

        appendStringInfoString(&select_cmd, "SELECT ");

        if (opts->num_masks > 0 && opts->num_columns == 0)
        {
            /*
             * Masking without explicit column list: query source catalog
             * for all column names so we can apply mask expressions to
             * specific columns while passing others through unchanged.
             */
            StringInfoData col_query;
            PGresult *col_res;
            int ncols, ci;

            initStringInfo(&col_query);
            appendStringInfo(&col_query,
                "SELECT a.attname "
                "FROM pg_catalog.pg_attribute a "
                "JOIN pg_catalog.pg_class c ON c.oid = a.attrelid "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "WHERE n.nspname = %s AND c.relname = %s "
                "AND a.attnum > 0 AND NOT a.attisdropped "
                "ORDER BY a.attnum",
                quote_literal_cstr(schema_name),
                quote_literal_cstr(source_table));

            col_res = PQexec(source_conn, col_query.data);
            pfree(col_query.data);

            if (PQresultStatus(col_res) != PGRES_TUPLES_OK)
            {
                char *col_errmsg = pstrdup(PQerrorMessage(source_conn));
                PQclear(col_res);
                ereport(ERROR,
                        (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                         errmsg("pgclone: could not fetch column list for masking: %s",
                                col_errmsg)));
            }

            ncols = PQntuples(col_res);
            for (ci = 0; ci < ncols; ci++)
            {
                const char *col_name = PQgetvalue(col_res, ci, 0);
                const char *col_ident = quote_identifier(col_name);
                const MaskRule *rule = pgclone_find_mask_rule(opts, col_name);

                if (ci > 0)
                    appendStringInfoString(&select_cmd, ", ");

                if (rule != NULL)
                {
                    pgclone_build_mask_expr(&select_cmd, col_ident, rule);
                    /* Alias to original column name so COPY IN matches */
                    appendStringInfo(&select_cmd, " AS %s", col_ident);
                }
                else
                {
                    appendStringInfoString(&select_cmd, col_ident);
                }
            }
            PQclear(col_res);
        }
        else if (opts->num_columns > 0)
        {
            /* Explicit column list — apply masks to matching columns */
            int ci;
            for (ci = 0; ci < opts->num_columns; ci++)
            {
                const char *col_ident = quote_identifier(opts->columns[ci]);
                const MaskRule *rule = pgclone_find_mask_rule(opts, opts->columns[ci]);

                if (ci > 0)
                    appendStringInfoString(&select_cmd, ", ");

                if (rule != NULL)
                {
                    pgclone_build_mask_expr(&select_cmd, col_ident, rule);
                    appendStringInfo(&select_cmd, " AS %s", col_ident);
                }
                else
                {
                    appendStringInfoString(&select_cmd, col_ident);
                }
            }
        }
        else
        {
            appendStringInfoChar(&select_cmd, '*');
        }

        appendStringInfo(&select_cmd, " FROM %s.%s",
                         quote_identifier(schema_name),
                         quote_identifier(source_table));

        if (opts->where_clause[0] != '\0')
            appendStringInfo(&select_cmd, " WHERE %s", opts->where_clause);

        appendStringInfo(&cmd, "COPY (%s) TO STDOUT WITH (FORMAT text)",
                         select_cmd.data);

        elog(DEBUG1, "pgclone: masked COPY command: %s", select_cmd.data);
        pfree(select_cmd.data);
    }
    else
    {
        appendStringInfo(&cmd,
            "COPY %s.%s TO STDOUT WITH (FORMAT text)",
            quote_identifier(schema_name),
            quote_identifier(source_table));
    }

    res = PQexec(source_conn, cmd.data);

    if (PQresultStatus(res) != PGRES_COPY_OUT)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(source_conn));
        PQclear(res);
        pfree(cmd.data);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: COPY OUT failed on source: %s", copy_errmsg)));
    }
    PQclear(res);

    /* Start COPY IN on local — with column list if selective */
    resetStringInfo(&cmd);

    if (opts != NULL && opts->num_columns > 0)
    {
        int ci;
        appendStringInfo(&cmd, "COPY %s.%s (",
                         quote_identifier(schema_name),
                         quote_identifier(target_table));
        for (ci = 0; ci < opts->num_columns; ci++)
        {
            if (ci > 0)
                appendStringInfoString(&cmd, ", ");
            appendStringInfo(&cmd, "%s", quote_identifier(opts->columns[ci]));
        }
        appendStringInfoString(&cmd, ") FROM STDIN WITH (FORMAT text)");
    }
    else
    {
        appendStringInfo(&cmd,
            "COPY %s.%s FROM STDIN WITH (FORMAT text)",
            quote_identifier(schema_name),
            quote_identifier(target_table));
    }

    res = PQexec(local_conn, cmd.data);
    pfree(cmd.data);

    if (PQresultStatus(res) != PGRES_COPY_IN)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
        PQclear(res);

        PQgetCopyData(source_conn, &buf, 0);
        if (buf) PQfreemem(buf);

        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: COPY IN failed on local: %s", copy_errmsg)));
    }
    PQclear(res);

    /* Stream data from source -> local */
    while ((ret = PQgetCopyData(source_conn, &buf, 0)) > 0)
    {
        if (PQputCopyData(local_conn, buf, ret) != 1)
        {
            char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
            PQfreemem(buf);
            PQputCopyEnd(local_conn, "aborted");
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("pgclone: error writing COPY data to local: %s",
                            copy_errmsg)));
        }

        bytes_transferred += ret;
        PQfreemem(buf);
        row_count++;

        if (row_count % 50000 == 0)
        {
            CHECK_FOR_INTERRUPTS();
            elog(DEBUG1, "pgclone: ... %ld rows transferred so far",
                 (long) row_count);
        }
    }

    if (ret == -2)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(source_conn));
        PQputCopyEnd(local_conn, "source error");
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: COPY stream error from source: %s",
                        copy_errmsg)));
    }

    if (PQputCopyEnd(local_conn, NULL) != 1)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: error finalizing COPY on local: %s",
                        copy_errmsg)));
    }

    res = PQgetResult(local_conn);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        char *copy_errmsg = pstrdup(PQerrorMessage(local_conn));
        PQclear(res);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: COPY completed with error: %s",
                        copy_errmsg)));
    }

    row_count = atol(PQcmdTuples(res));
    PQclear(res);

    /* Close read-only transaction if WHERE clause was used */
    if (opts != NULL && opts->where_clause[0] != '\0')
    {
        PGresult *txres = PQexec(source_conn, "COMMIT");
        PQclear(txres);
    }

    return row_count;
}

/* ---------------------------------------------------------------
 * Internal helper: clone indexes for a table from source.
 * --------------------------------------------------------------- */
static int
pgclone_indexes(PGconn *source_conn, PGconn *target_conn,
                  const char *schema_name, const char *source_table,
                  const char *target_table,
                  const CloneOptions *opts)
{
    PGresult       *res;
    StringInfoData  buf;
    int             i, count, created = 0;

    initStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT pg_get_indexdef(i.indexrelid) AS indexdef, "
        "ARRAY(SELECT a.attname FROM pg_catalog.pg_attribute a "
        "      WHERE a.attrelid = c.oid "
        "      AND a.attnum = ANY(i.indkey) "
        "      AND a.attnum > 0) AS index_cols "
        "FROM pg_catalog.pg_index i "
        "JOIN pg_catalog.pg_class c ON c.oid = i.indrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relname = %s "
        "AND NOT i.indisprimary "
        "AND NOT EXISTS ("
        "    SELECT 1 FROM pg_catalog.pg_constraint con "
        "    WHERE con.conindid = i.indexrelid"
        ")",
        quote_literal_cstr(schema_name),
        quote_literal_cstr(source_table));

    res = pgclone_exec(source_conn, buf.data);

    count = PQntuples(res);
    for (i = 0; i < count; i++)
    {
        char           *indexdef  = pstrdup(PQgetvalue(res, i, 0));
        char           *idx_cols  = PQgetvalue(res, i, 1);
        StringInfoData  final_def;

        /* If selective column clone, skip indexes on columns not in target */
        if (opts != NULL && opts->num_columns > 0)
        {
            /* idx_cols is a postgres array like {col1,col2} */
            char *p = idx_cols;
            bool  skip = false;

            /* Strip leading { and trailing } */
            if (*p == '{') p++;
            {
                char col_copy[NAMEDATALEN * 8];
                char *tok;
                strncpy(col_copy, p, sizeof(col_copy) - 1);
                col_copy[sizeof(col_copy) - 1] = '\0';
                /* Remove trailing } */
                {
                    size_t clen = strlen(col_copy);
                    if (clen > 0 && col_copy[clen - 1] == '}')
                        col_copy[clen - 1] = '\0';
                }
                tok = strtok(col_copy, ",");
                while (tok != NULL && !skip)
                {
                    int cj;
                    bool found = false;
                    for (cj = 0; cj < opts->num_columns; cj++)
                    {
                        if (strcmp(tok, opts->columns[cj]) == 0)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        skip = true;
                    tok = strtok(NULL, ",");
                }
            }
            if (skip)
            {
                pfree(indexdef);
                continue;
            }
        }

        initStringInfo(&final_def);

        /* Inject IF NOT EXISTS and replace table name if needed */
        {
            char *p = indexdef;

            if (strncmp(p, "CREATE UNIQUE INDEX ", 20) == 0)
            {
                appendStringInfoString(&final_def, "CREATE UNIQUE INDEX IF NOT EXISTS ");
                p += 20;
            }
            else if (strncmp(p, "CREATE INDEX ", 13) == 0)
            {
                appendStringInfoString(&final_def, "CREATE INDEX IF NOT EXISTS ");
                p += 13;
            }
            else
            {
                appendStringInfoString(&final_def, p);
                p += strlen(p);
            }

            if (strcmp(source_table, target_table) != 0)
            {
                char  search_str[NAMEDATALEN * 2 + 8];
                char *pos;

                snprintf(search_str, sizeof(search_str), " ON %s.%s ",
                         quote_identifier(schema_name),
                         quote_identifier(source_table));

                pos = strstr(p, search_str);
                if (pos != NULL)
                {
                    appendBinaryStringInfo(&final_def, p, pos - p);
                    appendStringInfo(&final_def, " ON %s.%s ",
                                     quote_identifier(schema_name),
                                     quote_identifier(target_table));
                    appendStringInfoString(&final_def, pos + strlen(search_str));
                }
                else
                {
                    appendStringInfoString(&final_def, p);
                }
            }
            else
            {
                appendStringInfoString(&final_def, p);
            }
        }

        if (pgclone_exec_conn(target_conn, final_def.data))
            created++;

        pfree(final_def.data);
        pfree(indexdef);
    }

    PQclear(res);
    pfree(buf.data);

    return created;
}

/* ---------------------------------------------------------------
 * Internal helper: clone constraints for a table from source.
 * --------------------------------------------------------------- */
static int
pgclone_constraints(PGconn *source_conn, PGconn *target_conn,
                      const char *schema_name, const char *source_table,
                      const char *target_table,
                      const CloneOptions *opts)
{
    PGresult       *res;
    StringInfoData  buf;
    int             i, count, created = 0;

    initStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT conname, contype, "
        "pg_get_constraintdef(con.oid, true) AS condef "
        "FROM pg_catalog.pg_constraint con "
        "JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relname = %s "
        "AND con.contype != 'n' "  /* NOT NULL is handled by table DDL already */
        "AND con.contype != 'f' "  /* FK constraints handled separately after all tables created */
        "ORDER BY "
        "  CASE contype "
        "    WHEN 'p' THEN 1 "
        "    WHEN 'u' THEN 2 "
        "    WHEN 'x' THEN 3 "  /* EXCLUSION constraints */
        "    WHEN 'c' THEN 4 "
        "    WHEN 'f' THEN 5 "
        "    ELSE 6 "
        "  END, conname",
        quote_literal_cstr(schema_name),
        quote_literal_cstr(source_table));

    res = pgclone_exec(source_conn, buf.data);

    count = PQntuples(res);
    for (i = 0; i < count; i++)
    {
        const char *conname = PQgetvalue(res, i, 0);
        const char *condef  = PQgetvalue(res, i, 2);

        /* Skip if constraint already exists on target */
        {
            StringInfoData chk_buf;
            PGresult      *chk;
            bool           already_exists;

            initStringInfo(&chk_buf);
            appendStringInfo(&chk_buf,
                "SELECT 1 FROM pg_catalog.pg_constraint con "
                "JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "WHERE n.nspname = %s AND c.relname = %s AND con.conname = %s",
                quote_literal_cstr(schema_name),
                quote_literal_cstr(target_table),
                quote_literal_cstr(conname));
            chk = PQexec(target_conn, chk_buf.data);
            already_exists = (PQresultStatus(chk) == PGRES_TUPLES_OK &&
                              PQntuples(chk) > 0);
            PQclear(chk);
            pfree(chk_buf.data);
            if (already_exists)
                continue;
        }

        /*
         * If selective column cloning, skip constraints that reference
         * columns not present in the target table.
         */
        if (opts != NULL && opts->num_columns > 0)
        {
            PGresult       *col_chk;
            StringInfoData  col_buf;
            bool            cols_missing = false;

            initStringInfo(&col_buf);
            appendStringInfo(&col_buf,
                "SELECT a.attname "
                "FROM pg_catalog.pg_constraint con "
                "JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid "
                "  AND a.attnum = ANY(con.conkey) "
                "WHERE n.nspname = %s AND c.relname = %s AND con.conname = %s",
                quote_literal_cstr(schema_name),
                quote_literal_cstr(source_table),
                quote_literal_cstr(conname));

            col_chk = pgclone_exec(source_conn, col_buf.data);
            pfree(col_buf.data);

            if (PQresultStatus(col_chk) == PGRES_TUPLES_OK)
            {
                int ci, cj;
                for (ci = 0; ci < PQntuples(col_chk) && !cols_missing; ci++)
                {
                    const char *col = PQgetvalue(col_chk, ci, 0);
                    bool found = false;
                    for (cj = 0; cj < opts->num_columns; cj++)
                    {
                        if (strcmp(col, opts->columns[cj]) == 0)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        cols_missing = true;
                }
            }
            PQclear(col_chk);

            if (cols_missing)
                continue;
        }

        /*
         * If cloning to a different table name, rename the constraint
         * to avoid conflicts with constraints on other tables.
         * e.g. "simple_test_pkey" -> "simple_test_copy_pkey"
         */
        {
            char       new_conname[NAMEDATALEN * 2];
            const char *effective_conname = conname;

            if (strcmp(source_table, target_table) != 0)
            {
                /* Replace source_table prefix in constraint name */
                size_t src_len = strlen(source_table);
                if (strncmp(conname, source_table, src_len) == 0)
                {
                    snprintf(new_conname, sizeof(new_conname), "%s%s",
                             target_table, conname + src_len);
                    effective_conname = new_conname;
                }
            }

            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "ALTER TABLE %s.%s ADD CONSTRAINT %s %s",
                quote_identifier(schema_name),
                quote_identifier(target_table),
                quote_identifier(effective_conname),
                condef);
        }

        if (pgclone_exec_conn(target_conn, buf.data))
            created++;
    }

    PQclear(res);
    pfree(buf.data);

    return created;
}

/* ---------------------------------------------------------------
 * Internal helper: clone triggers for a table from source.
 * --------------------------------------------------------------- */
static int
pgclone_triggers(PGconn *source_conn, PGconn *target_conn,
                   const char *schema_name, const char *source_table,
                   const char *target_table)
{
    PGresult       *res;
    StringInfoData  buf;
    int             i, count, created = 0;

    initStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT pg_get_triggerdef(t.oid, true) AS trigdef "
        "FROM pg_catalog.pg_trigger t "
        "JOIN pg_catalog.pg_class c ON c.oid = t.tgrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relname = %s "
        "AND NOT t.tgisinternal "
        "ORDER BY t.tgname",
        quote_literal_cstr(schema_name),
        quote_literal_cstr(source_table));

    res = pgclone_exec(source_conn, buf.data);

    count = PQntuples(res);
    for (i = 0; i < count; i++)
    {
        char *trigdef = PQgetvalue(res, i, 0);

        if (strcmp(source_table, target_table) != 0)
        {
            StringInfoData  new_def;
            char           *pos;
            char            search_str[NAMEDATALEN * 2 + 8];

            snprintf(search_str, sizeof(search_str), " ON %s.%s ",
                     quote_identifier(schema_name),
                     quote_identifier(source_table));

            pos = strstr(trigdef, search_str);
            if (pos != NULL)
            {
                initStringInfo(&new_def);
                appendBinaryStringInfo(&new_def, trigdef, pos - trigdef);
                appendStringInfo(&new_def, " ON %s.%s ",
                                 quote_identifier(schema_name),
                                 quote_identifier(target_table));
                appendStringInfoString(&new_def, pos + strlen(search_str));

                if (pgclone_exec_conn(target_conn, new_def.data))
                    created++;
                pfree(new_def.data);
            }
            else
            {
                if (pgclone_exec_conn(target_conn, trigdef))
                    created++;
            }
        }
        else
        {
            if (pgclone_exec_conn(target_conn, trigdef))
                created++;
        }
    }

    PQclear(res);
    pfree(buf.data);

    return created;
}

/* ===============================================================
 * FUNCTION: pgclone_table
 *
 * Overloads:
 *   pgclone_table(conninfo, schema, table, include_data)
 *   pgclone_table(conninfo, schema, table, include_data, target_name)
 *   pgclone_table(conninfo, schema, table, include_data, target_name, options_json)
 *   pgclone_table_ex(conninfo, schema, table, include_data, target_name,
 *                      include_indexes, include_constraints, include_triggers)
 *
 * All variants go through the same C function. Arguments are
 * detected by PG_NARGS() and PG_ARGISNULL().
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_table);

Datum
pgclone_table(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    text       *tablename_t       = PG_GETARG_TEXT_PP(2);
    bool        include_data      = PG_GETARG_BOOL(3);

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);
    char       *table_name        = text_to_cstring(tablename_t);
    char       *target_name;
    CloneOptions opts             = pgclone_default_options();

    PGconn     *source_conn;
    PGconn     *local_conn;
    PGresult   *res;
    StringInfoData buf;

    /* Arg 4: target table name (optional) */
    if (PG_NARGS() >= 5 && !PG_ARGISNULL(4))
        target_name = text_to_cstring(PG_GETARG_TEXT_PP(4));
    else
        target_name = table_name;

    /* Arg 5: options — could be JSON text (6 args) */
    if (PG_NARGS() == 6 && !PG_ARGISNULL(5))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(5));
        opts = pgclone_parse_options(options_json);
        pfree(options_json);
    }

    /* Args 5,6,7: boolean overload (8 args via pgclone_table_ex) */
    if (PG_NARGS() == 8)
    {
        if (!PG_ARGISNULL(5))
            opts.include_indexes = PG_GETARG_BOOL(5);
        if (!PG_ARGISNULL(6))
            opts.include_constraints = PG_GETARG_BOOL(6);
        if (!PG_ARGISNULL(7))
            opts.include_triggers = PG_GETARG_BOOL(7);
    }

    /* Connect to source */
    source_conn = pgclone_connect(source_conninfo);

    /* ---- Step 1: Get CREATE TABLE DDL from source ---- */
    initStringInfo(&buf);

    if (opts.num_columns > 0)
    {
        /* Build column filter: AND a.attname IN ('col1', 'col2', ...) */
        StringInfoData col_filter;
        int ci;

        initStringInfo(&col_filter);
        appendStringInfoString(&col_filter, "AND a.attname IN (");
        for (ci = 0; ci < opts.num_columns; ci++)
        {
            if (ci > 0)
                appendStringInfoString(&col_filter, ", ");
            appendStringInfo(&col_filter, "%s",
                             quote_literal_cstr(opts.columns[ci]));
        }
        appendStringInfoChar(&col_filter, ')');

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
            "WHERE n.nspname = %s AND c.relname = %s "
            "AND a.attnum > 0 AND NOT a.attisdropped "
            "%s "
            "GROUP BY c.relname",
            quote_identifier(schema_name),
            quote_identifier(target_name),
            quote_literal_cstr(schema_name),
            quote_literal_cstr(table_name),
            col_filter.data);

        pfree(col_filter.data);
    }
    else
    {
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
            "WHERE n.nspname = %s AND c.relname = %s "
            "AND a.attnum > 0 AND NOT a.attisdropped "
            "GROUP BY c.relname",
            quote_identifier(schema_name),
            quote_identifier(target_name),
            quote_literal_cstr(schema_name),
            quote_literal_cstr(table_name));
    }

    res = pgclone_exec(source_conn, buf.data);

    if (PQntuples(res) == 0)
    {
        PQclear(res);
        PQfinish(source_conn);
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("pgclone: table \"%s.%s\" not found on source",
                        schema_name, table_name)));
    }

    /* Use loopback libpq connection for ALL local operations */
    local_conn = pgclone_connect_local();

    /* ---- Step 1b: Create schema locally ---- */
    {
        resetStringInfo(&buf);
        appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                         quote_identifier(schema_name));
        pgclone_exec_conn(local_conn, buf.data);
    }

    /* ---- Step 1c: Pre-create sequences this table depends on ---- */
    {
        PGresult *seq_res;
        int       si, nseqs;

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
            "FROM pg_catalog.pg_class t "
            "JOIN pg_catalog.pg_namespace n ON n.oid = t.relnamespace "
            "JOIN pg_catalog.pg_depend dep ON dep.refobjid = t.oid "
            "                              AND dep.deptype = 'a' "
            "                              AND dep.classid = 'pg_catalog.pg_class'::regclass "
            "JOIN pg_catalog.pg_class s ON s.oid = dep.objid AND s.relkind = 'S' "
            "JOIN pg_catalog.pg_sequence ON pg_sequence.seqrelid = s.oid "
            "WHERE n.nspname = %s AND t.relname = %s",
            quote_literal_cstr(schema_name),
            quote_literal_cstr(table_name));

        seq_res = pgclone_exec(source_conn, buf.data);
        nseqs   = PQntuples(seq_res);

        for (si = 0; si < nseqs; si++)
        {
            PGresult *lcres;

            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "CREATE SEQUENCE IF NOT EXISTS %s.%s "
                "AS %s "
                "START WITH %s INCREMENT BY %s "
                "MINVALUE %s MAXVALUE %s CACHE %s %s",
                quote_identifier(schema_name),
                quote_identifier(PQgetvalue(seq_res, si, 0)),
                PQgetvalue(seq_res, si, 7),    /* data type */
                PQgetvalue(seq_res, si, 1),    /* start */
                PQgetvalue(seq_res, si, 2),    /* increment */
                PQgetvalue(seq_res, si, 4),    /* min */
                PQgetvalue(seq_res, si, 3),    /* max */
                PQgetvalue(seq_res, si, 5),    /* cache */
                strcmp(PQgetvalue(seq_res, si, 6), "t") == 0 ? "CYCLE" : "NO CYCLE");

            lcres = PQexec(local_conn, buf.data);
            if (PQresultStatus(lcres) != PGRES_COMMAND_OK)
                ereport(WARNING,
                        (errmsg("pgclone: could not create sequence %s.%s: %s",
                                schema_name, PQgetvalue(seq_res, si, 0),
                                PQerrorMessage(local_conn))));
            PQclear(lcres);
        }
        PQclear(seq_res);
    }

    /* ---- Step 2: Create table via loopback ---- */
    {
        PGresult *local_res;
        char     *ddl;

        ddl = pstrdup(PQgetvalue(res, 0, 0));
        PQclear(res);

        local_res = PQexec(local_conn, ddl);
        if (PQresultStatus(local_res) != PGRES_COMMAND_OK)
        {
            char *ddl_errmsg = pstrdup(PQerrorMessage(local_conn));
            PQclear(local_res);
            PQfinish(local_conn);
            PQfinish(source_conn);
            pfree(ddl);
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("pgclone: failed to create table locally: %s",
                            ddl_errmsg)));
        }
        PQclear(local_res);
        pfree(ddl);
    }

    /* ---- Step 3: Copy data if requested ---- */
    if (include_data)
    {
        int64 row_count;

        row_count = pgclone_copy_data(source_conn, local_conn,
                                        schema_name, table_name, target_name,
                                        &opts);

        elog(DEBUG1, "pgclone: copied %ld rows into %s.%s using COPY protocol",
             (long) row_count, schema_name, target_name);
    }

    /* ---- Step 4: Clone constraints if enabled ---- */
    if (opts.include_constraints)
    {
        int con_count = pgclone_constraints(source_conn, local_conn,
                                              schema_name, table_name, target_name,
                                              &opts);
        if (con_count > 0)
            elog(DEBUG1, "pgclone: cloned %d constraints for %s.%s",
                 con_count, schema_name, target_name);
    }

    /* ---- Step 5: Clone indexes if enabled ---- */
    if (opts.include_indexes)
    {
        int idx_count = pgclone_indexes(source_conn, local_conn,
                                          schema_name, table_name, target_name,
                                          &opts);
        if (idx_count > 0)
            elog(DEBUG1, "pgclone: cloned %d indexes for %s.%s",
                 idx_count, schema_name, target_name);
    }

    /* ---- Step 6: Clone triggers if enabled ---- */
    if (opts.include_triggers)
    {
        int trig_count = pgclone_triggers(source_conn, local_conn,
                                            schema_name, table_name, target_name);
        if (trig_count > 0)
            elog(DEBUG1, "pgclone: cloned %d triggers for %s.%s",
                 trig_count, schema_name, target_name);
    }

    PQfinish(local_conn);
    PQfinish(source_conn);

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgclone_table_ex
 *
 * Boolean overload: separate params for indexes, constraints, triggers
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_table_ex);

Datum
pgclone_table_ex(PG_FUNCTION_ARGS)
{
    /* Just forward to pgclone_table — same C function handles both */
    return pgclone_table(fcinfo);
}

/* ===============================================================
 * FUNCTION: pgclone_schema(source_conninfo, schema, include_data [, options])
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_schema);

Datum
pgclone_schema(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    bool        include_data      = PG_GETARG_BOOL(2);
    CloneOptions opts             = pgclone_default_options();

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);

    PGconn     *source_conn;
    PGconn     *local_conn;
    PGresult   *res;
    StringInfoData buf;
    int         i, ntables;
    char      **table_names = NULL;

    /* Arg 3: JSON options (only for the 4-arg overload where arg 3 is TEXT).
     * MUST use == 4, NOT >= 4:  when called from pgclone_schema_ex with
     * 6 args, arg 3 is a BOOLEAN — calling PG_GETARG_TEXT_PP on it would
     * dereference an invalid pointer and crash the server (SIGSEGV). */
    if (PG_NARGS() == 4 && !PG_ARGISNULL(3))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(3));
        opts = pgclone_parse_options(options_json);
        pfree(options_json);
    }

    /* Arg 3,4,5: boolean overload (6 args via pgclone_schema_ex) */
    if (PG_NARGS() == 6)
    {
        if (!PG_ARGISNULL(3))
            opts.include_indexes = PG_GETARG_BOOL(3);
        if (!PG_ARGISNULL(4))
            opts.include_constraints = PG_GETARG_BOOL(4);
        if (!PG_ARGISNULL(5))
            opts.include_triggers = PG_GETARG_BOOL(5);
    }

    source_conn = pgclone_connect(source_conninfo);
    local_conn = pgclone_connect_local();

    /* ---- Step 1: Create schema ---- */
    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                     quote_identifier(schema_name));
    pgclone_exec_conn(local_conn, buf.data);

    /* ---- Step 2: Clone sequences ----
     *
     * Sequences must be created before tables because a column DEFAULT
     * may use nextval('seq_name') and that reference is resolved at
     * CREATE TABLE time (against the local connection's search_path).
     * Functions used to be cloned here too — they have been moved to
     * Step 7 (after tables and views exist) to avoid SQL-language
     * function bodies referencing tables that don't yet exist.
     */
    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT sequence_name, data_type, start_value, increment, "
        "minimum_value, maximum_value, cycle_option "
        "FROM information_schema.sequences "
        "WHERE sequence_schema = %s",
        quote_literal_cstr(schema_name));

    res = pgclone_exec(source_conn, buf.data);

    for (i = 0; i < PQntuples(res); i++)
    {
        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "CREATE SEQUENCE IF NOT EXISTS %s.%s "
            "AS %s START WITH %s INCREMENT BY %s "
            "MINVALUE %s MAXVALUE %s %s",
            quote_identifier(schema_name),
            quote_identifier(PQgetvalue(res, i, 0)),
            PQgetvalue(res, i, 1),
            PQgetvalue(res, i, 2),
            PQgetvalue(res, i, 3),
            PQgetvalue(res, i, 4),
            PQgetvalue(res, i, 5),
            strcmp(PQgetvalue(res, i, 6), "YES") == 0 ? "CYCLE" : "NO CYCLE");
        pgclone_exec_conn(local_conn, buf.data);
    }
    elog(DEBUG1, "pgclone: cloned %d sequences from schema %s",
         PQntuples(res), schema_name);
    PQclear(res);

    PQfinish(local_conn);

    /* ---- Step 3: Clone tables ----
     *
     * Triggers are deferred to Step 8 by passing triggers=false through
     * to each pgclone_table() call.  This avoids two ordering hazards:
     *   1. A trigger's EXECUTE FUNCTION target may live in the same
     *      schema and only get cloned in Step 7.
     *   2. A trigger may be on a partitioned parent whose partition
     *      hasn't been cloned yet.
     * The user-requested include_triggers flag is preserved in
     * `opts.include_triggers` and consulted again in Step 8.
     */
    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT tablename FROM pg_catalog.pg_tables "
        "WHERE schemaname = %s ORDER BY tablename",
        quote_literal_cstr(schema_name));

    res = pgclone_exec(source_conn, buf.data);

    ntables = PQntuples(res);

    if (ntables > 0)
    {
        table_names = palloc(sizeof(char *) * ntables);
        for (i = 0; i < ntables; i++)
            table_names[i] = pstrdup(PQgetvalue(res, i, 0));
    }
    PQclear(res);
    PQfinish(source_conn);

    /* Build options JSON to pass through to pgclone_table.
     * triggers is forced to false here regardless of opts.include_triggers
     * — Step 8 handles the user-requested trigger pass after functions
     * exist. */
    {
        StringInfoData opts_json;
        initStringInfo(&opts_json);
        appendStringInfo(&opts_json,
            "{\"indexes\": %s, \"constraints\": %s, \"triggers\": false}",
            opts.include_indexes ? "true" : "false",
            opts.include_constraints ? "true" : "false");

        for (i = 0; i < ntables; i++)
        {
            Datum result;

            /* Call 6-arg version: conninfo, schema, table, include_data, target_name, options */
            result = DirectFunctionCall6(pgclone_table,
                        CStringGetTextDatum(source_conninfo),
                        CStringGetTextDatum(schema_name),
                        CStringGetTextDatum(table_names[i]),
                        BoolGetDatum(include_data),
                        CStringGetTextDatum(table_names[i]),  /* target = source name */
                        CStringGetTextDatum(opts_json.data));
            (void) result;
        }

        pfree(opts_json.data);
    }

    elog(DEBUG1, "pgclone: cloned %d tables from schema %s",
         ntables, schema_name);

    /* ---- Step 4: Retry FK constraints if constraints enabled ---- */
    if (opts.include_constraints)
    {
        PGconn *src_retry  = pgclone_connect(source_conninfo);
        PGconn *lcl_retry  = pgclone_connect_local();
        int     fk_created = 0;

        for (i = 0; i < ntables; i++)
        {
            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "SELECT conname, pg_get_constraintdef(con.oid, true) AS condef "
                "FROM pg_catalog.pg_constraint con "
                "JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "WHERE n.nspname = %s AND c.relname = %s "
                "AND contype = 'f'",
                quote_literal_cstr(schema_name),
                quote_literal_cstr(table_names[i]));

            res = pgclone_exec(src_retry, buf.data);

            {
                int j;
                for (j = 0; j < PQntuples(res); j++)
                {
                    const char *conname = PQgetvalue(res, j, 0);
                    const char *condef  = PQgetvalue(res, j, 1);

                    resetStringInfo(&buf);
                    appendStringInfo(&buf,
                        "DO $$ BEGIN "
                        "ALTER TABLE %s.%s ADD CONSTRAINT %s %s; "
                        "EXCEPTION WHEN duplicate_object THEN NULL; "
                        "END $$",
                        quote_identifier(schema_name),
                        quote_identifier(table_names[i]),
                        quote_identifier(conname),
                        condef);

                    if (pgclone_exec_conn(lcl_retry, buf.data))
                        fk_created++;
                }
            }

            PQclear(res);
        }

        if (fk_created > 0)
            elog(DEBUG1, "pgclone: FK retry pass: ensured %d foreign key constraints in schema %s",
                 fk_created, schema_name);

        PQfinish(lcl_retry);
        PQfinish(src_retry);
    }

    /* ---- Step 5: Clone views ---- */
    {
        PGconn *src_views = pgclone_connect(source_conninfo);
        PGconn *lcl_views = pgclone_connect_local();

        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "SELECT table_name, view_definition "
            "FROM information_schema.views "
            "WHERE table_schema = %s",
            quote_literal_cstr(schema_name));

        res = pgclone_exec(src_views, buf.data);

        for (i = 0; i < PQntuples(res); i++)
        {
            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "CREATE OR REPLACE VIEW %s.%s AS %s",
                quote_identifier(schema_name),
                quote_identifier(PQgetvalue(res, i, 0)),
                PQgetvalue(res, i, 1));
            pgclone_exec_conn(lcl_views, buf.data);
        }
        elog(DEBUG1, "pgclone: cloned %d views from schema %s",
             PQntuples(res), schema_name);
        PQclear(res);

        /* ---- Step 6: Clone materialized views ---- */
        if (opts.include_matviews)
        {
            resetStringInfo(&buf);
            appendStringInfo(&buf,
                "SELECT c.relname AS matview_name, "
                "pg_get_viewdef(c.oid, true) AS matview_def "
                "FROM pg_catalog.pg_class c "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "WHERE n.nspname = %s AND c.relkind = 'm' "
                "ORDER BY c.relname",
                quote_literal_cstr(schema_name));

            res = pgclone_exec(src_views, buf.data);

            for (i = 0; i < PQntuples(res); i++)
            {
                const char *mv_name = PQgetvalue(res, i, 0);
                const char *mv_def  = PQgetvalue(res, i, 1);

                /* Create the materialized view — strip trailing semicolon if present */
                {
                    char *mv_def_clean = pstrdup(mv_def);
                    size_t mv_len = strlen(mv_def_clean);
                    while (mv_len > 0 &&
                           (mv_def_clean[mv_len - 1] == ';' ||
                            mv_def_clean[mv_len - 1] == ' ' ||
                            mv_def_clean[mv_len - 1] == '\n'))
                        mv_def_clean[--mv_len] = '\0';

                    resetStringInfo(&buf);
                    appendStringInfo(&buf,
                        "CREATE MATERIALIZED VIEW IF NOT EXISTS %s.%s AS %s WITH DATA",
                        quote_identifier(schema_name),
                        quote_identifier(mv_name),
                        mv_def_clean);
                    pgclone_exec_conn(lcl_views, buf.data);
                    pfree(mv_def_clean);
                }

                /* Clone indexes on materialized view */
                if (opts.include_indexes)
                {
                    PGresult *idx_res;
                    int       idx_i;

                    resetStringInfo(&buf);
                    appendStringInfo(&buf,
                        "SELECT pg_get_indexdef(i.indexrelid) "
                        "FROM pg_catalog.pg_index i "
                        "JOIN pg_catalog.pg_class c ON c.oid = i.indrelid "
                        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                        "WHERE n.nspname = %s AND c.relname = %s",
                        quote_literal_cstr(schema_name),
                        quote_literal_cstr(mv_name));

                    idx_res = pgclone_exec(src_views, buf.data);
                    for (idx_i = 0; idx_i < PQntuples(idx_res); idx_i++)
                        pgclone_exec_conn(lcl_views, PQgetvalue(idx_res, idx_i, 0));
                    PQclear(idx_res);
                }
            }

            elog(DEBUG1, "pgclone: cloned %d materialized views from schema %s",
                 PQntuples(res), schema_name);
            PQclear(res);
        }

        PQfinish(lcl_views);
        PQfinish(src_views);
    }

    /* ---- Step 7: Clone functions/procedures ----
     *
     * Deferred until tables, views, and matviews exist so that
     * SQL-language function bodies (which validate object references
     * at CREATE FUNCTION time) don't fail on forward references to
     * objects in this schema.  pg_get_functiondef() already emits the
     * CREATE OR REPLACE FUNCTION header schema-qualified; the body is
     * reproduced verbatim from pg_proc.prosrc, so qualification of
     * references inside the body is whatever the original author wrote.
     */
    {
        PGconn *src_funcs = pgclone_connect(source_conninfo);
        PGconn *lcl_funcs = pgclone_connect_local();
        int     fcount;

        resetStringInfo(&buf);
        appendStringInfo(&buf,
            "SELECT pg_get_functiondef(p.oid) AS funcdef "
            "FROM pg_catalog.pg_proc p "
            "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
            "WHERE n.nspname = %s",
            quote_literal_cstr(schema_name));

        res = pgclone_exec(src_funcs, buf.data);
        fcount = PQntuples(res);

        for (i = 0; i < fcount; i++)
            pgclone_exec_conn(lcl_funcs, PQgetvalue(res, i, 0));

        elog(DEBUG1, "pgclone: cloned %d functions from schema %s",
             fcount, schema_name);
        PQclear(res);

        PQfinish(lcl_funcs);
        PQfinish(src_funcs);
    }

    /* ---- Step 8: Triggers (deferred from Step 3) ----
     *
     * Functions exist now (Step 7), so trigger DDLs that reference an
     * EXECUTE FUNCTION target in the same schema can be replayed safely.
     * We loop over the same table_names[] gathered in Step 3, opening
     * a single source/local pair to amortize connection cost.
     */
    if (opts.include_triggers && ntables > 0)
    {
        PGconn *src_trig = pgclone_connect(source_conninfo);
        PGconn *lcl_trig = pgclone_connect_local();
        int     trig_total = 0;

        for (i = 0; i < ntables; i++)
        {
            int n = pgclone_triggers(src_trig, lcl_trig,
                                       schema_name, table_names[i],
                                       table_names[i]);
            trig_total += n;
        }

        if (trig_total > 0)
            elog(DEBUG1, "pgclone: trigger pass: created %d triggers in schema %s",
                 trig_total, schema_name);

        PQfinish(lcl_trig);
        PQfinish(src_trig);
    }

    if (table_names)
    {
        for (i = 0; i < ntables; i++)
            pfree(table_names[i]);
        pfree(table_names);
    }

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* pgclone_schema_ex — boolean overload for schema */
PG_FUNCTION_INFO_V1(pgclone_schema_ex);

Datum
pgclone_schema_ex(PG_FUNCTION_ARGS)
{
    return pgclone_schema(fcinfo);
}

/* ===============================================================
 * FUNCTION: pgclone_functions(source_conninfo, schema)
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_functions);

Datum
pgclone_functions(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);

    PGconn     *source_conn;
    PGconn     *local_conn;
    PGresult   *res;
    StringInfoData buf;
    int         i, count;

    source_conn = pgclone_connect(source_conninfo);
    local_conn  = pgclone_connect_local();

    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s",
                     quote_identifier(schema_name));
    pgclone_exec_conn(local_conn, buf.data);

    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT pg_get_functiondef(p.oid) AS funcdef "
        "FROM pg_catalog.pg_proc p "
        "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
        "WHERE n.nspname = %s",
        quote_literal_cstr(schema_name));

    res = pgclone_exec(source_conn, buf.data);

    count = PQntuples(res);
    for (i = 0; i < count; i++)
    {
        pgclone_exec_conn(local_conn, PQgetvalue(res, i, 0));
    }

    PQclear(res);
    PQfinish(local_conn);
    PQfinish(source_conn);

    elog(DEBUG1, "pgclone: successfully cloned %d functions from %s",
         count, schema_name);

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgclone_database(source_conninfo, include_data [, options])
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_database);

Datum
pgclone_database(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    bool        include_data      = PG_GETARG_BOOL(1);
    CloneOptions opts             = pgclone_default_options();

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);

    PGconn     *source_conn;
    PGresult   *res;
    int         i, nschemas;
    char      **schema_names;

    /* Arg 2: JSON options (optional) */
    if (PG_NARGS() >= 3 && !PG_ARGISNULL(2))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(2));
        opts = pgclone_parse_options(options_json);
        pfree(options_json);
    }

    source_conn = pgclone_connect(source_conninfo);

    res = pgclone_exec(source_conn,
        "SELECT nspname FROM pg_catalog.pg_namespace "
        "WHERE nspname NOT LIKE 'pg_%' "
        "AND nspname <> 'information_schema' "
        "ORDER BY nspname");

    nschemas = PQntuples(res);

    schema_names = palloc(sizeof(char *) * nschemas);
    for (i = 0; i < nschemas; i++)
        schema_names[i] = pstrdup(PQgetvalue(res, i, 0));

    PQclear(res);
    PQfinish(source_conn);

    {
        StringInfoData opts_json;
        initStringInfo(&opts_json);
        appendStringInfo(&opts_json,
            "{\"indexes\": %s, \"constraints\": %s, \"triggers\": %s}",
            opts.include_indexes ? "true" : "false",
            opts.include_constraints ? "true" : "false",
            opts.include_triggers ? "true" : "false");

        for (i = 0; i < nschemas; i++)
        {
            Datum result;

            elog(DEBUG1, "pgclone: cloning schema %s (%d/%d)",
                 schema_names[i], i + 1, nschemas);

            result = DirectFunctionCall4(pgclone_schema,
                        CStringGetTextDatum(source_conninfo),
                        CStringGetTextDatum(schema_names[i]),
                        BoolGetDatum(include_data),
                        CStringGetTextDatum(opts_json.data));
            (void) result;
        }

        pfree(opts_json.data);
    }

    ereport(NOTICE,
            (errmsg("pgclone: database clone complete — %d schemas cloned",
                    nschemas)));

    for (i = 0; i < nschemas; i++)
        pfree(schema_names[i]);
    pfree(schema_names);

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * FUNCTION: pgclone_database_create(source_conninfo, target_dbname
 *              [, include_data [, options]])
 *
 * Creates the target database locally if it does not exist,
 * installs the pgclone extension in it, then delegates to
 * pgclone_database() running inside the target database so that
 * all schemas, tables, functions, etc. are cloned from the
 * remote source into the freshly created local database.
 *
 * Must be called from any local database (typically "postgres").
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_database_create);

Datum
pgclone_database_create(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *target_dbname_t   = PG_GETARG_TEXT_PP(1);
    bool        include_data      = true;
    char       *options_json      = NULL;

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *target_dbname     = text_to_cstring(target_dbname_t);

    PGconn     *admin_conn;       /* connection to local postgres DB */
    PGconn     *target_conn;      /* connection to local target DB   */
    PGresult   *res;
    StringInfoData buf;
    const char *port;
    const char *username;

    /* Optional arg 2: include_data */
    if (PG_NARGS() >= 3 && !PG_ARGISNULL(2))
        include_data = PG_GETARG_BOOL(2);

    /* Optional arg 3: JSON options */
    if (PG_NARGS() >= 4 && !PG_ARGISNULL(3))
        options_json = text_to_cstring(PG_GETARG_TEXT_PP(3));

    /* Validate target_dbname: must be a simple identifier */
    {
        int ci;
        for (ci = 0; target_dbname[ci] != '\0'; ci++)
        {
            char ch = target_dbname[ci];
            if (!((ch >= 'a' && ch <= 'z') ||
                  (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') ||
                  ch == '_'))
            {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("pgclone: invalid database name: %s", target_dbname)));
            }
        }
        if (ci == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("pgclone: database name cannot be empty")));
    }

    port = GetConfigOption("port", false, false);
    username = GetUserNameFromId(GetUserId(), false);

    /* ---- Step 1: Connect to local "postgres" DB ---- */
    initStringInfo(&buf);
    pgclone_append_local_host(&buf);
    appendStringInfo(&buf, " dbname=%s port=%s user=%s",
                     quote_literal_cstr("postgres"),
                     port ? port : "5432",
                     username);

    admin_conn = PQconnectdb(buf.data);
    if (PQstatus(admin_conn) != CONNECTION_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(admin_conn));
        PQfinish(admin_conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgclone: could not connect to postgres DB: %s",
                        errmsg_str)));
    }

    /* ---- Step 2: Check if target database exists ---- */
    resetStringInfo(&buf);
    appendStringInfo(&buf,
        "SELECT 1 FROM pg_catalog.pg_database WHERE datname = %s",
        quote_literal_cstr(target_dbname));

    res = PQexec(admin_conn, buf.data);
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(admin_conn));
        PQclear(res);
        PQfinish(admin_conn);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: could not check database existence: %s",
                        errmsg_str)));
    }

    if (PQntuples(res) == 0)
    {
        PQclear(res);

        /* CREATE DATABASE — cannot run inside a transaction */
        resetStringInfo(&buf);
        appendStringInfo(&buf, "CREATE DATABASE %s",
                         quote_identifier(target_dbname));

        res = PQexec(admin_conn, buf.data);
        if (PQresultStatus(res) != PGRES_COMMAND_OK)
        {
            char *errmsg_str = pstrdup(PQerrorMessage(admin_conn));
            PQclear(res);
            PQfinish(admin_conn);
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("pgclone: could not create database %s: %s",
                            target_dbname, errmsg_str)));
        }
        PQclear(res);

        ereport(NOTICE,
                (errmsg("pgclone: created database %s", target_dbname)));
    }
    else
    {
        PQclear(res);
        ereport(NOTICE,
                (errmsg("pgclone: database %s already exists, cloning into it",
                        target_dbname)));
    }

    PQfinish(admin_conn);

    /* ---- Step 3: Connect to target database ---- */
    resetStringInfo(&buf);
    pgclone_append_local_host(&buf);
    appendStringInfo(&buf, " dbname=%s port=%s user=%s",
                     quote_literal_cstr(target_dbname),
                     port ? port : "5432",
                     username);

    target_conn = PQconnectdb(buf.data);
    if (PQstatus(target_conn) != CONNECTION_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(target_conn));
        PQfinish(target_conn);
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("pgclone: could not connect to target database %s: %s",
                        target_dbname, errmsg_str)));
    }

    /* ---- Step 4: Install pgclone extension in target DB ---- */
    res = PQexec(target_conn, "CREATE EXTENSION IF NOT EXISTS pgclone");
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(target_conn));
        PQclear(res);
        PQfinish(target_conn);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: could not install pgclone in %s: %s",
                        target_dbname, errmsg_str)));
    }
    PQclear(res);

    ereport(NOTICE,
            (errmsg("pgclone: pgclone extension ready in %s", target_dbname)));

    /* ---- Step 5: Execute pgclone_database() inside target DB ---- */
    resetStringInfo(&buf);

    if (options_json != NULL)
    {
        /* 3-arg: pgclone.database(conninfo, include_data, options) */
        appendStringInfo(&buf,
            "SELECT pgclone.database(%s, %s, %s)",
            quote_literal_cstr(source_conninfo),
            include_data ? "true" : "false",
            quote_literal_cstr(options_json));
    }
    else
    {
        /* 2-arg: pgclone.database(conninfo, include_data) */
        appendStringInfo(&buf,
            "SELECT pgclone.database(%s, %s)",
            quote_literal_cstr(source_conninfo),
            include_data ? "true" : "false");
    }

    ereport(NOTICE,
            (errmsg("pgclone: starting database clone from source into %s ...",
                    target_dbname)));

    res = PQexec(target_conn, buf.data);
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(target_conn));
        PQclear(res);
        PQfinish(target_conn);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: database clone failed in %s: %s",
                        target_dbname, errmsg_str)));
    }
    PQclear(res);
    PQfinish(target_conn);

    pfree(buf.data);
    pfree(source_conninfo);
    pfree(target_dbname);
    if (options_json)
        pfree(options_json);

    ereport(NOTICE,
            (errmsg("pgclone: database clone complete — target database %s ready",
                    text_to_cstring(target_dbname_t))));

    PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
}

/* ===============================================================
 * Sensitivity pattern rules — shared by pgclone_discover_sensitive,
 * pgclone_masking_report, and future compliance functions.
 *
 * Each rule maps a column name pattern (LIKE-style) to a sensitivity
 * category and recommended masking strategy.
 * =============================================================== */
typedef struct SensitivityRule
{
    const char *pattern;     /* LIKE pattern: %email%, %_name, etc. */
    const char *category;    /* Human-readable: "Email", "PII - Name", etc. */
    const char *strategy;    /* Recommended mask type */
} SensitivityRule;

static const SensitivityRule sensitivity_rules[] = {
    /* Email */
    {"%email%",          "Email",              "email"},
    {"%e_mail%",         "Email",              "email"},
    /* Name / PII */
    {"%first_name%",     "PII - Name",         "name"},
    {"%last_name%",      "PII - Name",         "name"},
    {"%full_name%",      "PII - Name",         "name"},
    {"%firstname%",      "PII - Name",         "name"},
    {"%lastname%",       "PII - Name",         "name"},
    {"%fullname%",       "PII - Name",         "name"},
    {"%_name",           "PII - Name",         "name"},
    /* Phone */
    {"%phone%",          "Phone",              "phone"},
    {"%mobile%",         "Phone",              "phone"},
    {"%telephone%",      "Phone",              "phone"},
    {"%fax%",            "Phone",              "phone"},
    /* SSN / National ID */
    {"%ssn%",            "National ID",        "null"},
    {"%social_security%","National ID",        "null"},
    {"%national_id%",    "National ID",        "null"},
    {"%tax_id%",         "National ID",        "null"},
    {"%taxpayer%",       "National ID",        "null"},
    /* Financial */
    {"%salary%",         "Financial",          "random_int"},
    {"%income%",         "Financial",          "random_int"},
    {"%wage%",           "Financial",          "random_int"},
    {"%compensation%",   "Financial",          "random_int"},
    /* Credentials */
    {"%password%",       "Credential",         "hash"},
    {"%passwd%",         "Credential",         "hash"},
    {"%secret%",         "Credential",         "hash"},
    {"%token%",          "Credential",         "hash"},
    {"%api_key%",        "Credential",         "hash"},
    {"%apikey%",         "Credential",         "hash"},
    /* Address */
    {"%address%",        "Address",            "constant"},
    {"%street%",         "Address",            "constant"},
    {"%zip%",            "Address",            "partial"},
    {"%zipcode%",        "Address",            "partial"},
    {"%postal%",         "Address",            "partial"},
    /* Date of birth */
    {"%birth%",          "Date of Birth",      "null"},
    {"%dob%",            "Date of Birth",      "null"},
    {"%date_of_birth%",  "Date of Birth",      "null"},
    /* Credit card */
    {"%card_number%",    "Credit Card",        "null"},
    {"%credit_card%",    "Credit Card",        "null"},
    {"%ccn%",            "Credit Card",        "null"},
    {"%cvv%",            "Credit Card",        "null"},
    /* IP / location */
    {"%ip_address%",     "IP Address",         "hash"},
    {"%ipaddress%",      "IP Address",         "hash"},
    {NULL, NULL, NULL}
};

/* ---------------------------------------------------------------
 * Match a column name against sensitivity rules.
 * Returns the matching rule, or NULL if no match.
 * --------------------------------------------------------------- */
static const SensitivityRule *
pgclone_match_sensitivity(const char *col_name)
{
    char    col_lower[NAMEDATALEN];
    int     cl, ri;

    /* Lowercase the column name */
    for (cl = 0; col_name[cl] && cl < NAMEDATALEN - 1; cl++)
        col_lower[cl] = (col_name[cl] >= 'A' && col_name[cl] <= 'Z')
                        ? (col_name[cl] + 32) : col_name[cl];
    col_lower[cl] = '\0';

    for (ri = 0; sensitivity_rules[ri].pattern != NULL; ri++)
    {
        const char *pat = sensitivity_rules[ri].pattern;
        bool match = false;

        if (pat[0] == '%' && pat[strlen(pat) - 1] == '%')
        {
            /* %substring% — contains */
            char substr[128];
            int plen = strlen(pat) - 2;
            if (plen > 0 && plen < (int)sizeof(substr))
            {
                memcpy(substr, pat + 1, plen);
                substr[plen] = '\0';
                match = (strstr(col_lower, substr) != NULL);
            }
        }
        else if (pat[0] == '%')
        {
            /* %suffix — ends with */
            const char *suffix = pat + 1;
            int slen = strlen(suffix);
            int clen = strlen(col_lower);
            if (clen >= slen)
                match = (strcmp(col_lower + clen - slen, suffix) == 0);
        }

        if (match)
            return &sensitivity_rules[ri];
    }
    return NULL;
}

/* ===============================================================
 * FUNCTION: pgclone_discover_sensitive(conninfo, schema_name)
 *
 * Scans the source catalog for columns whose names match common
 * sensitive data patterns and returns a JSON "mask" object with
 * suggested masking strategies, ready to use in clone options.
 *
 * Pattern matching is case-insensitive against column names.
 * Only user columns (attnum > 0, not dropped) are inspected.
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_discover_sensitive);

Datum
pgclone_discover_sensitive(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);

    PGconn         *source_conn;
    PGresult       *res;
    StringInfoData  query;
    StringInfoData  result;
    int             nrows, i;
    bool            first_table = true;

    source_conn = pgclone_connect(source_conninfo);

    /*
     * Query all user tables and their columns in the given schema.
     * We fetch table_name + column_name pairs, then match patterns in C.
     */
    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT c.relname AS table_name, "
        "       a.attname AS column_name "
        "FROM pg_catalog.pg_class c "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid "
        "WHERE n.nspname = %s "
        "AND c.relkind IN ('r', 'p') "   /* regular tables + partitioned */
        "AND a.attnum > 0 AND NOT a.attisdropped "
        "ORDER BY c.relname, a.attnum",
        quote_literal_cstr(schema_name));

    res = pgclone_exec(source_conn, query.data);
    pfree(query.data);

    nrows = PQntuples(res);

    /*
     * Build JSON result grouped by table:
     * {
     *   "table1": {"col1": "email", "col2": "name"},
     *   "table2": {"col3": "phone"}
     * }
     */
    initStringInfo(&result);
    appendStringInfoChar(&result, '{');

    {
        char current_table[NAMEDATALEN];
        bool first_col_in_table = true;
        int  matched_in_table = 0;

        current_table[0] = '\0';

        for (i = 0; i < nrows; i++)
        {
            const char *tbl = PQgetvalue(res, i, 0);
            const char *col = PQgetvalue(res, i, 1);
            const SensitivityRule *rule;

            rule = pgclone_match_sensitivity(col);

            if (rule == NULL)
                continue;

            /* New table? Close previous table's object and open new one. */
            if (strcmp(tbl, current_table) != 0)
            {
                if (matched_in_table > 0)
                    appendStringInfoChar(&result, '}');

                if (!first_table)
                    appendStringInfoString(&result, ", ");

                appendStringInfo(&result, "\"%s\": {", tbl);
                strlcpy(current_table, tbl, NAMEDATALEN);
                first_table = false;
                first_col_in_table = true;
                matched_in_table = 0;
            }

            if (!first_col_in_table)
                appendStringInfoString(&result, ", ");

            appendStringInfo(&result, "\"%s\": \"%s\"",
                             col, rule->strategy);
            first_col_in_table = false;
            matched_in_table++;
        }

        /* Close last table object if any */
        if (matched_in_table > 0)
            appendStringInfoChar(&result, '}');
    }

    appendStringInfoChar(&result, '}');

    PQclear(res);
    PQfinish(source_conn);

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/* ===============================================================
 * FUNCTION: pgclone_mask_in_place(schema_name, table_name, mask_json)
 *
 * Applies data masking to an existing LOCAL table using UPDATE.
 * No source connection needed — works on already-cloned data.
 *
 * Uses a loopback libpq connection to execute:
 *   UPDATE schema.table SET col1 = mask_expr(col1), col2 = ...
 *
 * The mask_json is the same format as the "mask" option in clone:
 *   {"email": "email", "name": "name", "ssn": "null"}
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_mask_in_place);

Datum
pgclone_mask_in_place(PG_FUNCTION_ARGS)
{
    text       *schema_t    = PG_GETARG_TEXT_PP(0);
    text       *tablename_t = PG_GETARG_TEXT_PP(1);
    text       *mask_json_t = PG_GETARG_TEXT_PP(2);

    char       *schema_name = text_to_cstring(schema_t);
    char       *table_name  = text_to_cstring(tablename_t);
    char       *mask_json   = text_to_cstring(mask_json_t);

    PGconn         *local_conn;
    PGresult       *res;
    StringInfoData  update_cmd;
    StringInfoData  result;
    StringInfoData  wrapped;
    CloneOptions    opts;
    int             i;
    bool            first_set = true;
    int64           rows_affected;

    /* Wrap the mask JSON in a full options object for parsing */
    initStringInfo(&wrapped);
    appendStringInfo(&wrapped, "{\"mask\": %s}", mask_json);
    opts = pgclone_parse_options(wrapped.data);
    pfree(wrapped.data);

    if (opts.num_masks == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone: no valid mask rules in JSON"),
                 errhint("Provide mask rules like: {\"email\": \"email\", \"name\": \"name\"}")));

    /* Build UPDATE statement */
    initStringInfo(&update_cmd);
    appendStringInfo(&update_cmd, "UPDATE %s.%s SET ",
                     quote_identifier(schema_name),
                     quote_identifier(table_name));

    for (i = 0; i < opts.num_masks; i++)
    {
        const MaskRule *rule = &opts.masks[i];
        const char *col_ident = quote_identifier(rule->column);
        StringInfoData expr;

        if (!first_set)
            appendStringInfoString(&update_cmd, ", ");

        initStringInfo(&expr);
        pgclone_build_mask_expr(&expr, col_ident, rule);

        appendStringInfo(&update_cmd, "%s = %s", col_ident, expr.data);
        pfree(expr.data);
        first_set = false;
    }

    local_conn = pgclone_connect_local();

    res = PQexec(local_conn, update_cmd.data);

    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(local_conn));
        PQclear(res);
        PQfinish(local_conn);
        pfree(update_cmd.data);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: mask_in_place UPDATE failed: %s", errmsg_str)));
    }

    rows_affected = atol(PQcmdTuples(res));
    PQclear(res);
    PQfinish(local_conn);

    initStringInfo(&result);
    appendStringInfo(&result,
                     "OK: masked %ld rows in %s.%s (%d columns)",
                     (long) rows_affected,
                     schema_name, table_name,
                     opts.num_masks);

    pfree(update_cmd.data);

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/* ===============================================================
 * FUNCTION: pgclone_create_masking_policy(schema, table, mask_json,
 *                                         privileged_role)
 *
 * Creates a dynamic masking policy on a local table:
 *   1. Queries pg_attribute for all columns
 *   2. Creates a view (table_masked) with mask expressions applied
 *   3. Revokes SELECT on base table from PUBLIC
 *   4. Grants SELECT on masked view to PUBLIC
 *   5. Grants SELECT on base table to the privileged role
 *
 * Unprivileged users see masked data through the view.
 * The privileged role sees raw data directly from the table.
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_create_masking_policy);

Datum
pgclone_create_masking_policy(PG_FUNCTION_ARGS)
{
    text       *schema_t    = PG_GETARG_TEXT_PP(0);
    text       *tablename_t = PG_GETARG_TEXT_PP(1);
    text       *mask_json_t = PG_GETARG_TEXT_PP(2);
    text       *role_t      = PG_GETARG_TEXT_PP(3);

    char       *schema_name = text_to_cstring(schema_t);
    char       *table_name  = text_to_cstring(tablename_t);
    char       *mask_json   = text_to_cstring(mask_json_t);
    char       *priv_role   = text_to_cstring(role_t);

    PGconn         *local_conn;
    PGresult       *col_res;
    StringInfoData  wrapped;
    StringInfoData  col_query;
    StringInfoData  view_cmd;
    StringInfoData  result;
    CloneOptions    opts;
    int             ncols, ci;
    char            view_name[NAMEDATALEN * 2];

    /* Parse mask rules */
    initStringInfo(&wrapped);
    appendStringInfo(&wrapped, "{\"mask\": %s}", mask_json);
    opts = pgclone_parse_options(wrapped.data);
    pfree(wrapped.data);

    if (opts.num_masks == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pgclone: no valid mask rules in JSON"),
                 errhint("Provide mask rules like: {\"email\": \"email\", \"name\": \"name\"}")));

    /* Build view name: table_masked */
    snprintf(view_name, sizeof(view_name), "%s_masked", table_name);

    local_conn = pgclone_connect_local();

    /* Query local catalog for column names */
    initStringInfo(&col_query);
    appendStringInfo(&col_query,
        "SELECT a.attname "
        "FROM pg_catalog.pg_attribute a "
        "JOIN pg_catalog.pg_class c ON c.oid = a.attrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relname = %s "
        "AND a.attnum > 0 AND NOT a.attisdropped "
        "ORDER BY a.attnum",
        quote_literal_cstr(schema_name),
        quote_literal_cstr(table_name));

    col_res = PQexec(local_conn, col_query.data);
    pfree(col_query.data);

    if (PQresultStatus(col_res) != PGRES_TUPLES_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(local_conn));
        PQclear(col_res);
        PQfinish(local_conn);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: could not fetch column list: %s", errmsg_str)));
    }

    ncols = PQntuples(col_res);
    if (ncols == 0)
    {
        PQclear(col_res);
        PQfinish(local_conn);
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("pgclone: table \"%s.%s\" not found or has no columns",
                        schema_name, table_name)));
    }

    /* Build: CREATE OR REPLACE VIEW schema.table_masked AS
     *        SELECT mask(col1) AS col1, col2, ... FROM schema.table */
    initStringInfo(&view_cmd);
    appendStringInfo(&view_cmd,
        "CREATE OR REPLACE VIEW %s.%s AS SELECT ",
        quote_identifier(schema_name),
        quote_identifier(view_name));

    for (ci = 0; ci < ncols; ci++)
    {
        const char *col_name = PQgetvalue(col_res, ci, 0);
        const char *col_ident = quote_identifier(col_name);
        const MaskRule *rule = pgclone_find_mask_rule(&opts, col_name);

        if (ci > 0)
            appendStringInfoString(&view_cmd, ", ");

        if (rule != NULL)
        {
            pgclone_build_mask_expr(&view_cmd, col_ident, rule);
            appendStringInfo(&view_cmd, " AS %s", col_ident);
        }
        else
        {
            appendStringInfoString(&view_cmd, col_ident);
        }
    }

    appendStringInfo(&view_cmd, " FROM %s.%s",
                     quote_identifier(schema_name),
                     quote_identifier(table_name));

    PQclear(col_res);

    /* Step 1: Create the masked view */
    if (!pgclone_exec_conn(local_conn, view_cmd.data))
    {
        PQfinish(local_conn);
        pfree(view_cmd.data);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: failed to create masked view \"%s.%s\"",
                        schema_name, view_name)));
    }
    pfree(view_cmd.data);

    /* Step 2: Revoke direct table access from PUBLIC */
    {
        StringInfoData revoke_cmd;
        initStringInfo(&revoke_cmd);
        appendStringInfo(&revoke_cmd, "REVOKE SELECT ON %s.%s FROM PUBLIC",
                         quote_identifier(schema_name),
                         quote_identifier(table_name));
        pgclone_exec_conn(local_conn, revoke_cmd.data);
        pfree(revoke_cmd.data);
    }

    /* Step 3: Grant view access to PUBLIC */
    {
        StringInfoData grant_cmd;
        initStringInfo(&grant_cmd);
        appendStringInfo(&grant_cmd, "GRANT SELECT ON %s.%s TO PUBLIC",
                         quote_identifier(schema_name),
                         quote_identifier(view_name));
        pgclone_exec_conn(local_conn, grant_cmd.data);
        pfree(grant_cmd.data);
    }

    /* Step 4: Grant base table access to privileged role */
    {
        StringInfoData grant_priv;
        initStringInfo(&grant_priv);
        appendStringInfo(&grant_priv, "GRANT SELECT ON %s.%s TO %s",
                         quote_identifier(schema_name),
                         quote_identifier(table_name),
                         quote_identifier(priv_role));
        pgclone_exec_conn(local_conn, grant_priv.data);
        pfree(grant_priv.data);
    }

    PQfinish(local_conn);

    initStringInfo(&result);
    appendStringInfo(&result,
                     "OK: masking policy created on %s.%s "
                     "(view: %s.%s, privileged role: %s, %d masked columns)",
                     schema_name, table_name,
                     schema_name, view_name,
                     priv_role, opts.num_masks);

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/* ===============================================================
 * FUNCTION: pgclone_drop_masking_policy(schema, table)
 *
 * Removes a dynamic masking policy:
 *   1. Drops the masked view (table_masked)
 *   2. Re-grants SELECT on base table to PUBLIC
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_drop_masking_policy);

Datum
pgclone_drop_masking_policy(PG_FUNCTION_ARGS)
{
    text       *schema_t    = PG_GETARG_TEXT_PP(0);
    text       *tablename_t = PG_GETARG_TEXT_PP(1);

    char       *schema_name = text_to_cstring(schema_t);
    char       *table_name  = text_to_cstring(tablename_t);

    PGconn         *local_conn;
    StringInfoData  cmd;
    StringInfoData  result;
    char            view_name[NAMEDATALEN * 2];

    snprintf(view_name, sizeof(view_name), "%s_masked", table_name);

    local_conn = pgclone_connect_local();

    /* Drop the masked view */
    initStringInfo(&cmd);
    appendStringInfo(&cmd, "DROP VIEW IF EXISTS %s.%s",
                     quote_identifier(schema_name),
                     quote_identifier(view_name));

    if (!pgclone_exec_conn(local_conn, cmd.data))
    {
        PQfinish(local_conn);
        pfree(cmd.data);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: failed to drop masked view \"%s.%s\"",
                        schema_name, view_name)));
    }

    /* Re-grant SELECT on base table to PUBLIC */
    resetStringInfo(&cmd);
    appendStringInfo(&cmd, "GRANT SELECT ON %s.%s TO PUBLIC",
                     quote_identifier(schema_name),
                     quote_identifier(table_name));
    pgclone_exec_conn(local_conn, cmd.data);

    pfree(cmd.data);
    PQfinish(local_conn);

    initStringInfo(&result);
    appendStringInfo(&result,
                     "OK: masking policy removed from %s.%s (view %s.%s dropped)",
                     schema_name, table_name,
                     schema_name, view_name);

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/* ---------------------------------------------------------------
 * Internal helper: parse comma-separated role names into a SQL
 * IN clause fragment: "AND a.rolname IN ('role1', 'role2', ...)"
 * or "AND grantee IN ('role1', ...)" depending on col_name.
 *
 * Returns empty string if role_filter is NULL (no filtering).
 * Caller must pfree the returned string.
 * --------------------------------------------------------------- */
static char *
pgclone_build_role_filter(const char *role_filter, const char *col_name)
{
    StringInfoData  filter;
    const char     *p;
    bool            first = true;

    if (role_filter == NULL || role_filter[0] == '\0')
        return pstrdup("");

    initStringInfo(&filter);
    appendStringInfo(&filter, "AND %s IN (", col_name);

    p = role_filter;
    while (*p)
    {
        char    name[NAMEDATALEN];
        int     len = 0;

        /* Skip whitespace and commas */
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (*p == '\0') break;

        /* Extract role name */
        while (*p && *p != ',' && *p != ' ' && *p != '\t' && len < NAMEDATALEN - 1)
            name[len++] = *p++;
        name[len] = '\0';

        if (len > 0)
        {
            if (!first)
                appendStringInfoString(&filter, ", ");
            appendStringInfo(&filter, "%s", quote_literal_cstr(name));
            first = false;
        }
    }

    appendStringInfoChar(&filter, ')');

    /* If no valid names were parsed, return empty */
    if (first)
    {
        pfree(filter.data);
        return pstrdup("");
    }

    return filter.data;
}

/* ===============================================================
 * FUNCTION: pgclone_clone_roles(source_conninfo [, role_names])
 *
 * Clones database roles from source to local, including:
 *   - Role attributes (LOGIN, SUPERUSER, CREATEDB, etc.)
 *   - Encrypted passwords (copied as-is from pg_authid)
 *   - Connection limits and password validity
 *   - Role memberships (GRANT role TO role)
 *   - Schema-level privileges (USAGE, CREATE)
 *   - Table-level privileges (SELECT, INSERT, UPDATE, DELETE, etc.)
 *   - Sequence privileges (USAGE, SELECT, UPDATE)
 *   - Function/procedure EXECUTE privileges
 *
 * Overloads:
 *   pgclone_clone_roles(conninfo)           -- all non-system roles
 *   pgclone_clone_roles(conninfo, 'role1')  -- single role
 *   pgclone_clone_roles(conninfo, 'r1,r2')  -- specific roles
 *
 * If a role already exists on the target, its password and attributes
 * are synced to match the source. Permissions are applied additively
 * (existing grants are NOT revoked).
 *
 * Requires superuser on BOTH source and target (pg_authid access).
 * System roles (pg_*) and the postgres role are always excluded.
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_clone_roles);

Datum
pgclone_clone_roles(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *role_filter       = NULL;

    PGconn         *source_conn;
    PGconn         *local_conn;
    PGresult       *role_res;
    PGresult       *grant_res;
    StringInfoData  query;
    StringInfoData  cmd;
    StringInfoData  result;
    int             ngrants;
    int             i;
    int             roles_created = 0;
    int             roles_updated = 0;
    int             grants_applied = 0;

    /* Optional: comma-separated role names */
    if (PG_NARGS() >= 2 && !PG_ARGISNULL(1))
        role_filter = text_to_cstring(PG_GETARG_TEXT_PP(1));

    /* Build reusable filter fragments */
    char *authid_filter  = pgclone_build_role_filter(role_filter, "a.rolname");
    char *grantee_filter = pgclone_build_role_filter(role_filter, "grantee");
    char *member_filter  = pgclone_build_role_filter(role_filter, "r.rolname");
    char *acl_filter     = pgclone_build_role_filter(role_filter, "r.rolname");

    source_conn = pgclone_connect(source_conninfo);
    local_conn = pgclone_connect_local();

    /* ---- Step 1: Fetch roles from source pg_authid ---- */
    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT a.rolname, a.rolsuper, a.rolinherit, "
        "a.rolcreaterole, a.rolcreatedb, a.rolcanlogin, "
        "a.rolreplication, a.rolconnlimit, a.rolpassword, "
        "a.rolvaliduntil "
        "FROM pg_catalog.pg_authid a "
        "WHERE a.rolname NOT LIKE 'pg_%%%%' "
        "AND a.rolname <> 'postgres' "
        "%s "
        "ORDER BY a.rolname",
        authid_filter);

    role_res = PQexec(source_conn, query.data);
    resetStringInfo(&query);

    if (PQresultStatus(role_res) != PGRES_TUPLES_OK)
    {
        char *errmsg_str = pstrdup(PQerrorMessage(source_conn));
        PQclear(role_res);
        PQfinish(source_conn);
        PQfinish(local_conn);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pgclone: could not query pg_authid on source: %s", errmsg_str),
                 errhint("Requires superuser access on the source database.")));
    }

    /* ---- Step 2: Create/update each role on target ---- */
    initStringInfo(&cmd);

    for (i = 0; i < PQntuples(role_res); i++)
    {
        const char *rolname      = PQgetvalue(role_res, i, 0);
        const char *rolsuper     = PQgetvalue(role_res, i, 1);
        const char *rolinherit   = PQgetvalue(role_res, i, 2);
        const char *rolcreaterole= PQgetvalue(role_res, i, 3);
        const char *rolcreatedb  = PQgetvalue(role_res, i, 4);
        const char *rolcanlogin  = PQgetvalue(role_res, i, 5);
        const char *rolrepl      = PQgetvalue(role_res, i, 6);
        const char *rolconnlimit = PQgetvalue(role_res, i, 7);
        bool        has_password = !PQgetisnull(role_res, i, 8);
        const char *rolpassword  = has_password ? PQgetvalue(role_res, i, 8) : NULL;
        bool        has_validuntil = !PQgetisnull(role_res, i, 9);
        const char *rolvaliduntil  = has_validuntil ? PQgetvalue(role_res, i, 9) : NULL;
        PGresult   *check_res;
        bool        role_exists;

        /* Check if role already exists on target */
        resetStringInfo(&cmd);
        appendStringInfo(&cmd,
            "SELECT 1 FROM pg_catalog.pg_roles WHERE rolname = %s",
            quote_literal_cstr(rolname));

        check_res = PQexec(local_conn, cmd.data);
        role_exists = (PQresultStatus(check_res) == PGRES_TUPLES_OK &&
                       PQntuples(check_res) > 0);
        PQclear(check_res);

        /* CREATE or ALTER the role with all attributes */
        resetStringInfo(&cmd);
        if (!role_exists)
        {
            appendStringInfo(&cmd, "CREATE ROLE %s WITH %s %s %s %s %s %s CONNECTION LIMIT %s",
                             quote_identifier(rolname),
                             rolsuper[0] == 't' ? "SUPERUSER" : "NOSUPERUSER",
                             rolinherit[0] == 't' ? "INHERIT" : "NOINHERIT",
                             rolcreaterole[0] == 't' ? "CREATEROLE" : "NOCREATEROLE",
                             rolcreatedb[0] == 't' ? "CREATEDB" : "NOCREATEDB",
                             rolcanlogin[0] == 't' ? "LOGIN" : "NOLOGIN",
                             rolrepl[0] == 't' ? "REPLICATION" : "NOREPLICATION",
                             rolconnlimit);

            if (pgclone_exec_conn(local_conn, cmd.data))
                roles_created++;
            else
                elog(WARNING, "pgclone: failed to create role \"%s\"", rolname);
        }
        else
        {
            appendStringInfo(&cmd, "ALTER ROLE %s WITH %s %s %s %s %s %s CONNECTION LIMIT %s",
                             quote_identifier(rolname),
                             rolsuper[0] == 't' ? "SUPERUSER" : "NOSUPERUSER",
                             rolinherit[0] == 't' ? "INHERIT" : "NOINHERIT",
                             rolcreaterole[0] == 't' ? "CREATEROLE" : "NOCREATEROLE",
                             rolcreatedb[0] == 't' ? "CREATEDB" : "NOCREATEDB",
                             rolcanlogin[0] == 't' ? "LOGIN" : "NOLOGIN",
                             rolrepl[0] == 't' ? "REPLICATION" : "NOREPLICATION",
                             rolconnlimit);

            pgclone_exec_conn(local_conn, cmd.data);
            roles_updated++;
        }

        /* Sync password — the value from pg_authid is already encrypted
         * (SCRAM-SHA-256 or md5 hash). PostgreSQL recognizes the hash format
         * and stores it as-is without re-hashing. */
        if (has_password)
        {
            resetStringInfo(&cmd);
            appendStringInfo(&cmd, "ALTER ROLE %s PASSWORD %s",
                             quote_identifier(rolname),
                             quote_literal_cstr(rolpassword));
            pgclone_exec_conn(local_conn, cmd.data);
        }

        /* Sync password validity */
        if (has_validuntil)
        {
            resetStringInfo(&cmd);
            appendStringInfo(&cmd, "ALTER ROLE %s VALID UNTIL %s",
                             quote_identifier(rolname),
                             quote_literal_cstr(rolvaliduntil));
            pgclone_exec_conn(local_conn, cmd.data);
        }
    }

    PQclear(role_res);

    /* ---- Step 3: Clone role memberships (GRANT role TO role) ---- */
    appendStringInfo(&query,
        "SELECT r.rolname AS member, g.rolname AS group_role "
        "FROM pg_catalog.pg_auth_members m "
        "JOIN pg_catalog.pg_authid r ON r.oid = m.member "
        "JOIN pg_catalog.pg_authid g ON g.oid = m.roleid "
        "WHERE r.rolname NOT LIKE 'pg_%%%%' "
        "AND g.rolname NOT LIKE 'pg_%%%%' "
        "AND r.rolname <> 'postgres' "
        "AND g.rolname <> 'postgres' "
        "%s",
        member_filter);

    grant_res = PQexec(source_conn, query.data);
    resetStringInfo(&query);

    if (PQresultStatus(grant_res) == PGRES_TUPLES_OK)
    {
        ngrants = PQntuples(grant_res);
        for (i = 0; i < ngrants; i++)
        {
            const char *member = PQgetvalue(grant_res, i, 0);
            const char *grp    = PQgetvalue(grant_res, i, 1);

            resetStringInfo(&cmd);
            appendStringInfo(&cmd, "GRANT %s TO %s",
                             quote_identifier(grp),
                             quote_identifier(member));

            if (pgclone_exec_conn(local_conn, cmd.data))
                grants_applied++;
        }
    }
    PQclear(grant_res);

    /* ---- Step 4: Clone schema-level privileges ---- */
    appendStringInfo(&query,
        "SELECT n.nspname, "
        "ae.privilege_type AS priv, "
        "r.rolname AS grantee_name "
        "FROM pg_catalog.pg_namespace n, "
        "LATERAL aclexplode(n.nspacl) ae "
        "JOIN pg_catalog.pg_authid r ON r.oid = ae.grantee "
        "WHERE n.nspname NOT LIKE 'pg_%%%%' "
        "AND n.nspname <> 'information_schema' "
        "AND r.rolname NOT LIKE 'pg_%%%%' "
        "AND r.rolname <> 'postgres' "
        "AND n.nspacl IS NOT NULL "
        "%s",
        acl_filter);

    grant_res = PQexec(source_conn, query.data);
    resetStringInfo(&query);

    if (PQresultStatus(grant_res) == PGRES_TUPLES_OK)
    {
        ngrants = PQntuples(grant_res);
        for (i = 0; i < ngrants; i++)
        {
            const char *nsp     = PQgetvalue(grant_res, i, 0);
            const char *priv    = PQgetvalue(grant_res, i, 1);
            const char *grantee = PQgetvalue(grant_res, i, 2);

            resetStringInfo(&cmd);
            appendStringInfo(&cmd, "GRANT %s ON SCHEMA %s TO %s",
                             priv,
                             quote_identifier(nsp),
                             quote_identifier(grantee));

            if (pgclone_exec_conn(local_conn, cmd.data))
                grants_applied++;
        }
    }
    PQclear(grant_res);

    /* ---- Step 5: Clone table-level privileges ---- */
    appendStringInfo(&query,
        "SELECT table_schema, table_name, grantee, privilege_type "
        "FROM information_schema.table_privileges "
        "WHERE table_schema NOT LIKE 'pg_%%%%' "
        "AND table_schema <> 'information_schema' "
        "AND grantor <> grantee "
        "AND grantee NOT LIKE 'pg_%%%%' "
        "AND grantee <> 'postgres' "
        "%s "
        "ORDER BY table_schema, table_name, grantee",
        grantee_filter);

    grant_res = PQexec(source_conn, query.data);
    resetStringInfo(&query);

    if (PQresultStatus(grant_res) == PGRES_TUPLES_OK)
    {
        ngrants = PQntuples(grant_res);
        for (i = 0; i < ngrants; i++)
        {
            const char *tschema = PQgetvalue(grant_res, i, 0);
            const char *tname   = PQgetvalue(grant_res, i, 1);
            const char *grantee = PQgetvalue(grant_res, i, 2);
            const char *priv    = PQgetvalue(grant_res, i, 3);

            resetStringInfo(&cmd);
            appendStringInfo(&cmd, "GRANT %s ON %s.%s TO %s",
                             priv,
                             quote_identifier(tschema),
                             quote_identifier(tname),
                             quote_identifier(grantee));

            if (pgclone_exec_conn(local_conn, cmd.data))
                grants_applied++;
        }
    }
    PQclear(grant_res);

    /* ---- Step 6: Clone sequence privileges ---- */
    appendStringInfo(&query,
        "SELECT usg.object_schema, usg.object_name, usg.grantee, usg.privilege_type "
        "FROM information_schema.usage_privileges usg "
        "WHERE usg.object_schema NOT LIKE 'pg_%%%%' "
        "AND usg.object_schema <> 'information_schema' "
        "AND usg.object_type = 'SEQUENCE' "
        "AND usg.grantee NOT LIKE 'pg_%%%%' "
        "AND usg.grantee <> 'postgres' "
        "%s",
        grantee_filter);

    grant_res = PQexec(source_conn, query.data);
    resetStringInfo(&query);

    if (PQresultStatus(grant_res) == PGRES_TUPLES_OK)
    {
        ngrants = PQntuples(grant_res);
        for (i = 0; i < ngrants; i++)
        {
            const char *sschema = PQgetvalue(grant_res, i, 0);
            const char *sname   = PQgetvalue(grant_res, i, 1);
            const char *grantee = PQgetvalue(grant_res, i, 2);
            const char *priv    = PQgetvalue(grant_res, i, 3);

            resetStringInfo(&cmd);
            appendStringInfo(&cmd, "GRANT %s ON SEQUENCE %s.%s TO %s",
                             priv,
                             quote_identifier(sschema),
                             quote_identifier(sname),
                             quote_identifier(grantee));

            if (pgclone_exec_conn(local_conn, cmd.data))
                grants_applied++;
        }
    }
    PQclear(grant_res);

    /* ---- Step 7: Clone function/procedure EXECUTE privileges ---- */
    appendStringInfo(&query,
        "SELECT routine_schema, routine_name, grantee, privilege_type "
        "FROM information_schema.routine_privileges "
        "WHERE routine_schema NOT LIKE 'pg_%%%%' "
        "AND routine_schema <> 'information_schema' "
        "AND grantor <> grantee "
        "AND grantee NOT LIKE 'pg_%%%%' "
        "AND grantee <> 'postgres' "
        "%s",
        grantee_filter);

    grant_res = PQexec(source_conn, query.data);
    resetStringInfo(&query);

    if (PQresultStatus(grant_res) == PGRES_TUPLES_OK)
    {
        ngrants = PQntuples(grant_res);
        for (i = 0; i < ngrants; i++)
        {
            const char *fschema = PQgetvalue(grant_res, i, 0);
            const char *fname   = PQgetvalue(grant_res, i, 1);
            const char *grantee = PQgetvalue(grant_res, i, 2);
            const char *priv    = PQgetvalue(grant_res, i, 3);

            resetStringInfo(&cmd);
            appendStringInfo(&cmd, "GRANT %s ON FUNCTION %s.%s TO %s",
                             priv,
                             quote_identifier(fschema),
                             quote_identifier(fname),
                             quote_identifier(grantee));

            /* May fail for overloaded functions (ambiguous name) — OK */
            if (pgclone_exec_conn(local_conn, cmd.data))
                grants_applied++;
        }
    }
    PQclear(grant_res);

    pfree(cmd.data);
    pfree(query.data);
    pfree(authid_filter);
    pfree(grantee_filter);
    pfree(member_filter);
    pfree(acl_filter);
    PQfinish(source_conn);
    PQfinish(local_conn);

    initStringInfo(&result);
    appendStringInfo(&result,
                     "OK: %d roles created, %d roles updated, %d grants applied",
                     roles_created, roles_updated, grants_applied);

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/* ===============================================================
 * FUNCTION: pgclone_verify(source_conninfo [, schema_name])
 *
 * SET-RETURNING FUNCTION that compares row counts between source
 * and local (target) databases, table by table.
 *
 * Returns columns:
 *   schema_name, table_name, source_rows, target_rows, match
 *
 * Overloads:
 *   pgclone_verify(conninfo)           -- all user schemas
 *   pgclone_verify(conninfo, schema)   -- single schema
 *
 * "match" column values:
 *   '✓'           — row counts are equal
 *   '✗'           — row counts differ
 *   '✗ (missing)' — table exists on source but not on target
 * =============================================================== */

/* Per-row data stored during SRF iteration */
typedef struct VerifyRow
{
    char    schema_name[NAMEDATALEN];
    char    table_name[NAMEDATALEN];
    int64   source_rows;
    int64   target_rows;
    bool    target_exists;
} VerifyRow;

/* Context stored across SRF calls */
typedef struct VerifyState
{
    VerifyRow  *rows;
    int         num_rows;
    int         current_row;
} VerifyState;

#define PGCLONE_VERIFY_COLS 5

PG_FUNCTION_INFO_V1(pgclone_verify);

Datum
pgclone_verify(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext   oldctx;
        TupleDesc       tupdesc;
        VerifyState    *state;

        text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
        char       *source_conninfo   = text_to_cstring(source_conninfo_t);
        char       *schema_filter     = NULL;

        PGconn         *source_conn;
        PGconn         *local_conn;
        PGresult       *src_res;
        StringInfoData  query;
        int             ntables, i;

        if (PG_NARGS() >= 2 && !PG_ARGISNULL(1))
            schema_filter = text_to_cstring(PG_GETARG_TEXT_PP(1));

        funcctx = SRF_FIRSTCALL_INIT();
        oldctx  = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Build tuple descriptor */
        tupdesc = CreateTemplateTupleDesc(PGCLONE_VERIFY_COLS);
        TupleDescInitEntry(tupdesc, 1, "schema_name",  TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 2, "table_name",   TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 3, "source_rows",  INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 4, "target_rows",  INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 5, "match",        TEXTOID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        /* Connect to both source and local */
        source_conn = pgclone_connect(source_conninfo);
        local_conn = pgclone_connect_local();

        /*
         * Query source for all tables with row counts.
         * Uses pg_class.reltuples for fast approximate counts
         * (avoids full table scans). For exact counts after
         * ANALYZE, this is accurate enough for verification.
         */
        initStringInfo(&query);
        if (schema_filter != NULL)
        {
            appendStringInfo(&query,
                "SELECT n.nspname, c.relname, "
                "GREATEST(c.reltuples::bigint, 0) AS row_count "
                "FROM pg_catalog.pg_class c "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "WHERE c.relkind IN ('r', 'p') "
                "AND n.nspname = %s "
                "ORDER BY n.nspname, c.relname",
                quote_literal_cstr(schema_filter));
        }
        else
        {
            appendStringInfoString(&query,
                "SELECT n.nspname, c.relname, "
                "GREATEST(c.reltuples::bigint, 0) AS row_count "
                "FROM pg_catalog.pg_class c "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "WHERE c.relkind IN ('r', 'p') "
                "AND n.nspname NOT LIKE 'pg_%' "
                "AND n.nspname <> 'information_schema' "
                "ORDER BY n.nspname, c.relname");
        }

        src_res = PQexec(source_conn, query.data);

        if (PQresultStatus(src_res) != PGRES_TUPLES_OK)
        {
            char *errmsg_str = pstrdup(PQerrorMessage(source_conn));
            PQclear(src_res);
            PQfinish(source_conn);
            PQfinish(local_conn);
            pfree(query.data);
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("pgclone: could not query source tables: %s",
                            errmsg_str)));
        }

        ntables = PQntuples(src_res);

        /* Allocate state in multi_call context */
        state = palloc0(sizeof(VerifyState));
        state->rows = palloc0(sizeof(VerifyRow) * (ntables > 0 ? ntables : 1));
        state->num_rows = ntables;
        state->current_row = 0;

        /* For each source table, get local row count */
        for (i = 0; i < ntables; i++)
        {
            const char *nsp = PQgetvalue(src_res, i, 0);
            const char *tbl = PQgetvalue(src_res, i, 1);
            int64       src_count = atol(PQgetvalue(src_res, i, 2));
            PGresult   *local_res;
            StringInfoData local_query;

            strlcpy(state->rows[i].schema_name, nsp, NAMEDATALEN);
            strlcpy(state->rows[i].table_name, tbl, NAMEDATALEN);
            state->rows[i].source_rows = src_count;

            /* Check if table exists locally and get its row count */
            initStringInfo(&local_query);
            appendStringInfo(&local_query,
                "SELECT GREATEST(c.reltuples::bigint, 0) "
                "FROM pg_catalog.pg_class c "
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                "WHERE n.nspname = %s AND c.relname = %s "
                "AND c.relkind IN ('r', 'p')",
                quote_literal_cstr(nsp),
                quote_literal_cstr(tbl));

            local_res = PQexec(local_conn, local_query.data);
            pfree(local_query.data);

            if (PQresultStatus(local_res) == PGRES_TUPLES_OK &&
                PQntuples(local_res) > 0)
            {
                state->rows[i].target_rows = atol(PQgetvalue(local_res, 0, 0));
                state->rows[i].target_exists = true;
            }
            else
            {
                state->rows[i].target_rows = 0;
                state->rows[i].target_exists = false;
            }
            PQclear(local_res);
        }

        PQclear(src_res);
        pfree(query.data);
        PQfinish(source_conn);
        PQfinish(local_conn);

        funcctx->user_fctx = state;
        MemoryContextSwitchTo(oldctx);
    }

    funcctx = SRF_PERCALL_SETUP();

    {
        VerifyState *state = (VerifyState *) funcctx->user_fctx;

        if (state->current_row < state->num_rows)
        {
            VerifyRow  *row = &state->rows[state->current_row];
            Datum       values[PGCLONE_VERIFY_COLS];
            bool        nulls[PGCLONE_VERIFY_COLS];
            HeapTuple   tuple;
            const char *match_str;

            memset(nulls, 0, sizeof(nulls));

            values[0] = CStringGetTextDatum(row->schema_name);
            values[1] = CStringGetTextDatum(row->table_name);
            values[2] = Int64GetDatum(row->source_rows);
            values[3] = Int64GetDatum(row->target_rows);

            /* Determine match status */
            if (!row->target_exists)
                match_str = "\xe2\x9c\x97 (missing)";  /* ✗ (missing) */
            else if (row->source_rows == row->target_rows)
                match_str = "\xe2\x9c\x93";            /* ✓ */
            else
                match_str = "\xe2\x9c\x97";            /* ✗ */

            values[4] = CStringGetTextDatum(match_str);

            tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
            state->current_row++;

            SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
        }
    }

    SRF_RETURN_DONE(funcctx);
}

/* ===============================================================
 * FUNCTION: pgclone_masking_report(schema_name)
 *
 * SET-RETURNING FUNCTION that generates a GDPR/compliance audit
 * report for a local schema. For each table and column:
 *   1. Detects if the column name matches sensitive data patterns
 *   2. Checks if a masked view (table_masked) exists
 *   3. Reports the masking status and recommendation
 *
 * Returns columns:
 *   schema_name, table_name, column_name, sensitivity,
 *   mask_status, recommendation
 *
 * mask_status values:
 *   'MASKED (view)'  — a _masked view exists for this table
 *   'UNMASKED'       — sensitive column with no masking in place
 *   'NOT SENSITIVE'  — column doesn't match any sensitivity pattern
 * =============================================================== */

typedef struct ReportRow
{
    char    schema_name[NAMEDATALEN];
    char    table_name[NAMEDATALEN];
    char    column_name[NAMEDATALEN];
    char    sensitivity[64];     /* "Email", "PII - Name", etc. or "" */
    char    mask_status[32];     /* "MASKED (view)", "UNMASKED", "NOT SENSITIVE" */
    char    recommendation[128]; /* suggested action */
} ReportRow;

typedef struct ReportState
{
    ReportRow  *rows;
    int         num_rows;
    int         current_row;
} ReportState;

#define PGCLONE_REPORT_COLS 6

PG_FUNCTION_INFO_V1(pgclone_masking_report);

Datum
pgclone_masking_report(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext   oldctx;
        TupleDesc       tupdesc;
        ReportState    *state;

        text       *schema_t    = PG_GETARG_TEXT_PP(0);
        char       *schema_name = text_to_cstring(schema_t);

        PGconn         *local_conn;
        PGresult       *col_res;
        PGresult       *view_check;
        StringInfoData  query;
        StringInfoData  view_query;
        int             nrows, i;
        int             alloc_size;

        funcctx = SRF_FIRSTCALL_INIT();
        oldctx  = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        tupdesc = CreateTemplateTupleDesc(PGCLONE_REPORT_COLS);
        TupleDescInitEntry(tupdesc, 1, "schema_name",    TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 2, "table_name",     TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 3, "column_name",    TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 4, "sensitivity",    TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 5, "mask_status",    TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 6, "recommendation", TEXTOID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        local_conn = pgclone_connect_local();

        /* Get all columns in the schema */
        initStringInfo(&query);
        appendStringInfo(&query,
            "SELECT c.relname, a.attname "
            "FROM pg_catalog.pg_class c "
            "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
            "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid "
            "WHERE n.nspname = %s "
            "AND c.relkind IN ('r', 'p') "
            "AND a.attnum > 0 AND NOT a.attisdropped "
            "ORDER BY c.relname, a.attnum",
            quote_literal_cstr(schema_name));

        col_res = PQexec(local_conn, query.data);

        if (PQresultStatus(col_res) != PGRES_TUPLES_OK)
        {
            char *errmsg_str = pstrdup(PQerrorMessage(local_conn));
            PQclear(col_res);
            PQfinish(local_conn);
            pfree(query.data);
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("pgclone: could not query columns: %s", errmsg_str)));
        }

        nrows = PQntuples(col_res);
        alloc_size = nrows > 0 ? nrows : 1;

        state = palloc0(sizeof(ReportState));
        state->rows = palloc0(sizeof(ReportRow) * alloc_size);
        state->num_rows = 0;
        state->current_row = 0;

        /*
         * For each column, check sensitivity and whether a masked
         * view exists for its table.
         */
        initStringInfo(&view_query);

        {
            char last_table[NAMEDATALEN];
            bool last_table_has_view = false;

            last_table[0] = '\0';

            for (i = 0; i < nrows; i++)
            {
                const char *tbl = PQgetvalue(col_res, i, 0);
                const char *col = PQgetvalue(col_res, i, 1);
                const SensitivityRule *rule;
                ReportRow *row;
                bool is_sensitive;

                /* Check for masked view only when table changes */
                if (strcmp(tbl, last_table) != 0)
                {
                    strlcpy(last_table, tbl, NAMEDATALEN);

                    resetStringInfo(&view_query);
                    appendStringInfo(&view_query,
                        "SELECT 1 FROM pg_catalog.pg_class c "
                        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                        "WHERE n.nspname = %s AND c.relname = '%s_masked' "
                        "AND c.relkind = 'v'",
                        quote_literal_cstr(schema_name), tbl);

                    view_check = PQexec(local_conn, view_query.data);
                    last_table_has_view = (PQresultStatus(view_check) == PGRES_TUPLES_OK &&
                                           PQntuples(view_check) > 0);
                    PQclear(view_check);
                }

                rule = pgclone_match_sensitivity(col);
                is_sensitive = (rule != NULL);

                /* Only include sensitive columns in the report */
                if (!is_sensitive)
                    continue;

                row = &state->rows[state->num_rows];
                strlcpy(row->schema_name, schema_name, NAMEDATALEN);
                strlcpy(row->table_name, tbl, NAMEDATALEN);
                strlcpy(row->column_name, col, NAMEDATALEN);
                strlcpy(row->sensitivity, rule->category, sizeof(row->sensitivity));

                if (last_table_has_view)
                {
                    strlcpy(row->mask_status, "MASKED (view)", sizeof(row->mask_status));
                    snprintf(row->recommendation, sizeof(row->recommendation),
                             "OK - masked via %s_masked view", tbl);
                }
                else
                {
                    strlcpy(row->mask_status, "UNMASKED", sizeof(row->mask_status));
                    snprintf(row->recommendation, sizeof(row->recommendation),
                             "Apply mask strategy: %s", rule->strategy);
                }

                state->num_rows++;
            }
        }

        pfree(view_query.data);
        pfree(query.data);
        PQclear(col_res);
        PQfinish(local_conn);

        funcctx->user_fctx = state;
        MemoryContextSwitchTo(oldctx);
    }

    funcctx = SRF_PERCALL_SETUP();

    {
        ReportState *state = (ReportState *) funcctx->user_fctx;

        if (state->current_row < state->num_rows)
        {
            ReportRow  *row = &state->rows[state->current_row];
            Datum       values[PGCLONE_REPORT_COLS];
            bool        nulls[PGCLONE_REPORT_COLS];
            HeapTuple   tuple;

            memset(nulls, 0, sizeof(nulls));

            values[0] = CStringGetTextDatum(row->schema_name);
            values[1] = CStringGetTextDatum(row->table_name);
            values[2] = CStringGetTextDatum(row->column_name);
            values[3] = CStringGetTextDatum(row->sensitivity);
            values[4] = CStringGetTextDatum(row->mask_status);
            values[5] = CStringGetTextDatum(row->recommendation);

            tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
            state->current_row++;

            SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
        }
    }

    SRF_RETURN_DONE(funcctx);
}

/* ===============================================================
 * FUNCTION: pgclone_version()
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_version);

Datum
pgclone_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("pgclone 4.0.1"));
}

/* ===============================================================
 * _PG_init — called when the shared library is loaded.
 * Registers shared memory for job tracking.
 * Required: shared_preload_libraries = 'pgclone'
 * =============================================================== */
void _PG_init(void);

void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
        return;

    pgclone_shmem_init();
}

/* ===============================================================
 * ASYNC CLONE FUNCTIONS
 *
 * Submit clone jobs to background workers for non-blocking
 * operations with progress tracking, cancel, and resume.
 * =============================================================== */

/* ===============================================================
 * FUNCTION: pgclone_table_async(conninfo, schema, table,
 *              include_data [, target_name [, options_json]])
 *
 * Returns job_id (INTEGER).
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_table_async);

Datum
pgclone_table_async(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    text       *tablename_t       = PG_GETARG_TEXT_PP(2);
    bool        include_data      = PG_GETARG_BOOL(3);

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);
    char       *table_name        = text_to_cstring(tablename_t);
    char       *target_name       = table_name;
    CloneOptions opts             = pgclone_default_options();
    PgcloneConflictStrategy conflict = PGCLONE_CONFLICT_ERROR;

    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;
    PgcloneJob    *job;
    int             job_id;

    if (PG_NARGS() >= 5 && !PG_ARGISNULL(4))
        target_name = text_to_cstring(PG_GETARG_TEXT_PP(4));

    if (PG_NARGS() >= 6 && !PG_ARGISNULL(5))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(5));
        opts = pgclone_parse_options(options_json);

        if (strstr(options_json, "\"conflict\": \"skip\"") ||
            strstr(options_json, "\"conflict\":\"skip\""))
            conflict = PGCLONE_CONFLICT_SKIP;
        else if (strstr(options_json, "\"conflict\": \"replace\"") ||
                 strstr(options_json, "\"conflict\":\"replace\""))
            conflict = PGCLONE_CONFLICT_REPLACE;
        else if (strstr(options_json, "\"conflict\": \"rename\"") ||
                 strstr(options_json, "\"conflict\":\"rename\""))
            conflict = PGCLONE_CONFLICT_RENAME;

        pfree(options_json);
    }

    if (!pgclone_state)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pgclone: shared memory not initialized"),
                 errhint("Add pgclone to shared_preload_libraries in postgresql.conf")));

    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

    job = find_free_slot();
    if (!job)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                 errmsg("pgclone: no free job slots (max %d)", PGCLONE_MAX_JOBS)));
    }

    job_id = pgclone_state->next_job_id++;
    memset(job, 0, sizeof(PgcloneJob));
    job->job_id = job_id;
    job->status = PGCLONE_JOB_PENDING;
    job->op_type = PGCLONE_OP_TABLE;
    job->database_oid = MyDatabaseId;
    strlcpy(job->database_name, get_database_name(MyDatabaseId), NAMEDATALEN);
    strlcpy(job->username, GetUserNameFromId(GetUserId(), false), NAMEDATALEN);

    strlcpy(job->source_conninfo, source_conninfo, sizeof(job->source_conninfo));
    strlcpy(job->schema_name, schema_name, NAMEDATALEN);
    strlcpy(job->table_name, table_name, NAMEDATALEN);
    strlcpy(job->target_name, target_name, NAMEDATALEN);

    job->include_data        = include_data;
    job->include_indexes     = opts.include_indexes;
    job->include_constraints = opts.include_constraints;
    job->include_triggers    = opts.include_triggers;
    job->conflict_strategy   = conflict;
    job->resumable           = true;

    LWLockRelease(pgclone_state->lock);

    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "pgclone: job %d (%s.%s)",
             job_id, schema_name, table_name);
    snprintf(worker.bgw_type, BGW_MAXLEN, "pgclone worker");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgclone");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgclone_bgw_main");
    worker.bgw_main_arg = Int32GetDatum(job_id);
    worker.bgw_notify_pid = MyProcPid;

    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
    {
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        job->status = PGCLONE_JOB_FREE;
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: could not register background worker")));
    }

    /* Wait for the worker to actually start */
    {
        BgwHandleStatus status;
        pid_t           worker_pid;

        status = WaitForBackgroundWorkerStartup(handle, &worker_pid);
        if (status != BGWH_STARTED)
        {
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            job->status = PGCLONE_JOB_FAILED;
            strlcpy(job->error_message, "background worker failed to start", 256);
            job->end_time = GetCurrentTimestamp();
            LWLockRelease(pgclone_state->lock);
            ereport(ERROR,
                    (errmsg("pgclone: background worker failed to start (status=%d)", status)));
        }
    }

    PG_RETURN_INT32(job_id);
}

/* ===============================================================
 * FUNCTION: pgclone_schema_async(conninfo, schema, include_data
 *              [, options_json])
 *
 * When options contains "parallel": N (N > 1), launches N background
 * workers that each clone a subset of tables concurrently.
 * Without parallel, launches a single worker (as before).
 *
 * Returns the parent job_id. Child jobs are visible via pgclone_jobs().
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_schema_async);

Datum
pgclone_schema_async(PG_FUNCTION_ARGS)
{
    text       *source_conninfo_t = PG_GETARG_TEXT_PP(0);
    text       *schema_t          = PG_GETARG_TEXT_PP(1);
    bool        include_data      = PG_GETARG_BOOL(2);
    CloneOptions opts             = pgclone_default_options();
    PgcloneConflictStrategy conflict = PGCLONE_CONFLICT_ERROR;

    char       *source_conninfo   = text_to_cstring(source_conninfo_t);
    char       *schema_name       = text_to_cstring(schema_t);

    PgcloneJob    *job;
    int             job_id;

    if (PG_NARGS() >= 4 && !PG_ARGISNULL(3))
    {
        char *options_json = text_to_cstring(PG_GETARG_TEXT_PP(3));
        opts = pgclone_parse_options(options_json);

        if (strstr(options_json, "\"conflict\": \"skip\"") ||
            strstr(options_json, "\"conflict\":\"skip\""))
            conflict = PGCLONE_CONFLICT_SKIP;
        else if (strstr(options_json, "\"conflict\": \"replace\"") ||
                 strstr(options_json, "\"conflict\":\"replace\""))
            conflict = PGCLONE_CONFLICT_REPLACE;
        else if (strstr(options_json, "\"conflict\": \"rename\"") ||
                 strstr(options_json, "\"conflict\":\"rename\""))
            conflict = PGCLONE_CONFLICT_RENAME;

        pfree(options_json);
    }

    if (!pgclone_state)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pgclone: shared memory not initialized"),
                 errhint("Add pgclone to shared_preload_libraries")));

    if (opts.parallel_workers > 1)
    {
        /*
         * WORKER POOL MODE: Populate a shared task queue with the table
         * list, then launch exactly N pool workers. Each worker grabs
         * tasks from the queue until exhausted — no per-table bgworker.
         */
        PGconn     *source_conn;
        PGresult   *table_res;
        StringInfoData qbuf;
        int         ntables, wi;
        int         parent_job_id;
        int         workers_launched = 0;
        int         num_workers;

        /* Ensure no other pool operation is running */
        LWLockAcquire(pgclone_state->lock, LW_SHARED);
        if (pgclone_state->pool.active)
        {
            LWLockRelease(pgclone_state->lock);
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_IN_USE),
                     errmsg("pgclone: another pool operation is already running"),
                     errhint("Wait for the current parallel clone to finish or cancel it")));
        }
        LWLockRelease(pgclone_state->lock);

        /* Create schema + sequences via loopback */
        {
            PGconn     *local_conn = pgclone_connect_local();
            PGconn     *src_conn_pre = pgclone_connect(source_conninfo);

            StringInfoData fbuf;
            initStringInfo(&fbuf);
            appendStringInfo(&fbuf, "CREATE SCHEMA IF NOT EXISTS %s",
                             quote_identifier(schema_name));
            pgclone_exec_conn(local_conn, fbuf.data);

            /* Pre-create all sequences — pool workers need them for DEFAULT nextval() */
            {
                PGresult *seq_res;
                int si;

                resetStringInfo(&fbuf);
                appendStringInfo(&fbuf,
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
                    quote_literal_cstr(schema_name));

                seq_res = PQexec(src_conn_pre, fbuf.data);
                if (PQresultStatus(seq_res) == PGRES_TUPLES_OK)
                {
                    for (si = 0; si < PQntuples(seq_res); si++)
                    {
                        PGresult *lcres;

                        resetStringInfo(&fbuf);
                        appendStringInfo(&fbuf,
                            "CREATE SEQUENCE IF NOT EXISTS %s.%s "
                            "AS %s "
                            "START WITH %s INCREMENT BY %s "
                            "MINVALUE %s MAXVALUE %s CACHE %s %s",
                            quote_identifier(schema_name),
                            quote_identifier(PQgetvalue(seq_res, si, 0)),
                            PQgetvalue(seq_res, si, 7),
                            PQgetvalue(seq_res, si, 1),
                            PQgetvalue(seq_res, si, 2),
                            PQgetvalue(seq_res, si, 4),
                            PQgetvalue(seq_res, si, 3),
                            PQgetvalue(seq_res, si, 5),
                            strcmp(PQgetvalue(seq_res, si, 6), "t") == 0 ? "CYCLE" : "NO CYCLE");

                        lcres = PQexec(local_conn, fbuf.data);
                        if (PQresultStatus(lcres) != PGRES_COMMAND_OK)
                            ereport(WARNING,
                                    (errmsg("pgclone: pool pre-create sequence %s.%s: %s",
                                            schema_name, PQgetvalue(seq_res, si, 0),
                                            PQerrorMessage(local_conn))));
                        PQclear(lcres);
                    }
                }
                PQclear(seq_res);
            }

            PQfinish(src_conn_pre);
            PQfinish(local_conn);
            pfree(fbuf.data);
        }

        /* Get table list from source */
        source_conn = pgclone_connect(source_conninfo);

        initStringInfo(&qbuf);
        appendStringInfo(&qbuf,
            "SELECT tablename FROM pg_catalog.pg_tables "
            "WHERE schemaname = %s ORDER BY tablename",
            quote_literal_cstr(schema_name));

        table_res = pgclone_exec(source_conn, qbuf.data);
        ntables = PQntuples(table_res);

        PQfinish(source_conn);

        if (ntables == 0)
        {
            PQclear(table_res);
            pfree(qbuf.data);
            elog(DEBUG1, "pgclone: no tables found in schema %s", schema_name);
            PG_RETURN_INT32(0);
        }

        if (ntables > PGCLONE_MAX_POOL_TASKS)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pgclone: schema has %d tables, max pool queue is %d",
                            ntables, PGCLONE_MAX_POOL_TASKS)));

        /* Cap workers to table count */
        num_workers = (opts.parallel_workers > ntables)
            ? ntables : opts.parallel_workers;

        /* Populate pool queue and parent job under a single lock */
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

        job = find_free_slot();
        if (!job)
        {
            LWLockRelease(pgclone_state->lock);
            PQclear(table_res);
            pfree(qbuf.data);
            ereport(ERROR, (errmsg("pgclone: no free job slots")));
        }

        parent_job_id = pgclone_state->next_job_id++;
        memset(job, 0, sizeof(PgcloneJob));
        job->job_id = parent_job_id;
        job->status = PGCLONE_JOB_RUNNING;
        job->op_type = PGCLONE_OP_SCHEMA;
        job->database_oid = MyDatabaseId;
        strlcpy(job->database_name, get_database_name(MyDatabaseId), NAMEDATALEN);
        strlcpy(job->username, GetUserNameFromId(GetUserId(), false), NAMEDATALEN);
        job->total_tables = ntables;
        job->parallel_workers = num_workers;
        job->start_time = GetCurrentTimestamp();
        strlcpy(job->source_conninfo, source_conninfo, sizeof(job->source_conninfo));
        strlcpy(job->schema_name, schema_name, NAMEDATALEN);
        strlcpy(job->current_phase, "launching worker pool", 64);

        /* Fill the pool queue */
        memset(&pgclone_state->pool, 0, sizeof(PgclonePoolQueue));
        pgclone_state->pool.active = true;
        pgclone_state->pool.parent_job_id = parent_job_id;
        pgclone_state->pool.num_tasks = ntables;
        pgclone_state->pool.next_task_idx = 0;
        pgclone_state->pool.completed_count = 0;
        pgclone_state->pool.failed_count = 0;

        strlcpy(pgclone_state->pool.source_conninfo, source_conninfo,
                sizeof(pgclone_state->pool.source_conninfo));
        strlcpy(pgclone_state->pool.schema_name, schema_name, NAMEDATALEN);
        pgclone_state->pool.include_data = include_data;
        pgclone_state->pool.include_indexes = opts.include_indexes;
        pgclone_state->pool.include_constraints = opts.include_constraints;
        pgclone_state->pool.include_triggers = opts.include_triggers;
        pgclone_state->pool.conflict_strategy = conflict;
        pgclone_state->pool.database_oid = MyDatabaseId;
        strlcpy(pgclone_state->pool.database_name,
                get_database_name(MyDatabaseId), NAMEDATALEN);
        strlcpy(pgclone_state->pool.username,
                GetUserNameFromId(GetUserId(), false), NAMEDATALEN);

        {
            int ti;
            for (ti = 0; ti < ntables; ti++)
            {
                strlcpy(pgclone_state->pool.tasks[ti].table_name,
                        PQgetvalue(table_res, ti, 0), NAMEDATALEN);
                pgclone_state->pool.tasks[ti].status = 0; /* pending */
                pgclone_state->pool.tasks[ti].claimed_by_job_id = 0;
            }
        }

        LWLockRelease(pgclone_state->lock);

        PQclear(table_res);
        pfree(qbuf.data);

        /* Launch exactly N pool workers */
        for (wi = 0; wi < num_workers; wi++)
        {
            BackgroundWorker worker;
            BackgroundWorkerHandle *handle;
            PgcloneJob *worker_job;
            int worker_job_id;

            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

            worker_job = find_free_slot();
            if (!worker_job)
            {
                LWLockRelease(pgclone_state->lock);
                ereport(WARNING,
                        (errmsg("pgclone: no free job slot for pool worker %d", wi)));
                break;
            }

            worker_job_id = pgclone_state->next_job_id++;
            memset(worker_job, 0, sizeof(PgcloneJob));
            worker_job->job_id = worker_job_id;
            worker_job->status = PGCLONE_JOB_PENDING;
            worker_job->op_type = PGCLONE_OP_TABLE;
            worker_job->database_oid = MyDatabaseId;
            strlcpy(worker_job->database_name,
                    get_database_name(MyDatabaseId), NAMEDATALEN);
            strlcpy(worker_job->username,
                    GetUserNameFromId(GetUserId(), false), NAMEDATALEN);
            /* Mark as pool worker with sentinel value */
            worker_job->parallel_workers = -1;

            /* Register in pool's worker tracking array */
            pgclone_state->pool.worker_job_ids[pgclone_state->pool.num_workers] = worker_job_id;
            pgclone_state->pool.num_workers++;

            strlcpy(worker_job->source_conninfo, source_conninfo,
                    sizeof(worker_job->source_conninfo));
            strlcpy(worker_job->schema_name, schema_name, NAMEDATALEN);

            worker_job->include_data = include_data;
            worker_job->include_indexes = opts.include_indexes;
            worker_job->include_constraints = opts.include_constraints;
            worker_job->include_triggers = opts.include_triggers;
            worker_job->conflict_strategy = conflict;

            LWLockRelease(pgclone_state->lock);

            memset(&worker, 0, sizeof(BackgroundWorker));
            snprintf(worker.bgw_name, BGW_MAXLEN,
                     "pgclone: pool worker %d/%d (parent %d)",
                     wi + 1, num_workers, parent_job_id);
            snprintf(worker.bgw_type, BGW_MAXLEN, "pgclone pool worker");
            worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
            worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
            worker.bgw_restart_time = BGW_NEVER_RESTART;
            snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgclone");
            snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgclone_pool_worker_main");
            worker.bgw_main_arg = Int32GetDatum(worker_job_id);
            worker.bgw_notify_pid = MyProcPid;

            if (RegisterDynamicBackgroundWorker(&worker, &handle))
            {
                BgwHandleStatus wstatus;
                pid_t           wpid;
                wstatus = WaitForBackgroundWorkerStartup(handle, &wpid);
                if (wstatus == BGWH_STARTED)
                    workers_launched++;
                else
                    ereport(WARNING,
                            (errmsg("pgclone: pool worker %d failed to start", wi)));
            }
            else
                ereport(WARNING,
                        (errmsg("pgclone: could not register pool worker %d", wi)));
        }

        /* Update parent job with worker count */
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        {
            PgcloneJob *pj = find_job(parent_job_id);
            if (pj)
            {
                snprintf(pj->current_phase, 64,
                         "%d pool workers for %d tables", workers_launched, ntables);
                if (workers_launched == 0)
                {
                    pj->status = PGCLONE_JOB_FAILED;
                    pj->end_time = GetCurrentTimestamp();
                    pgclone_state->pool.active = false;
                }
            }
        }
        LWLockRelease(pgclone_state->lock);

        ereport(NOTICE,
                (errmsg("pgclone: launched %d pool workers for %d tables in schema %s (job %d)",
                        workers_launched, ntables, schema_name, parent_job_id)));

        PG_RETURN_INT32(parent_job_id);
    }
    else
    {
        /*
         * SEQUENTIAL MODE: Single worker for whole schema (original behavior)
         */
        BackgroundWorker worker;
        BackgroundWorkerHandle *handle;

        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

        job = find_free_slot();
        if (!job)
        {
            LWLockRelease(pgclone_state->lock);
            ereport(ERROR, (errmsg("pgclone: no free job slots")));
        }

        job_id = pgclone_state->next_job_id++;
        memset(job, 0, sizeof(PgcloneJob));
        job->job_id = job_id;
        job->status = PGCLONE_JOB_PENDING;
        job->op_type = PGCLONE_OP_SCHEMA;
        job->database_oid = MyDatabaseId;
        strlcpy(job->database_name, get_database_name(MyDatabaseId), NAMEDATALEN);
    strlcpy(job->username, GetUserNameFromId(GetUserId(), false), NAMEDATALEN);

        strlcpy(job->source_conninfo, source_conninfo, sizeof(job->source_conninfo));
        strlcpy(job->schema_name, schema_name, NAMEDATALEN);

        job->include_data        = include_data;
        job->include_indexes     = opts.include_indexes;
        job->include_constraints = opts.include_constraints;
        job->include_triggers    = opts.include_triggers;
        job->conflict_strategy   = conflict;
        job->resumable           = true;

        LWLockRelease(pgclone_state->lock);

        memset(&worker, 0, sizeof(BackgroundWorker));
        snprintf(worker.bgw_name, BGW_MAXLEN, "pgclone: schema %s (job %d)",
                 schema_name, job_id);
        snprintf(worker.bgw_type, BGW_MAXLEN, "pgclone worker");
        worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
        worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
        worker.bgw_restart_time = BGW_NEVER_RESTART;
        snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgclone");
        snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgclone_bgw_main");
        worker.bgw_main_arg = Int32GetDatum(job_id);
        worker.bgw_notify_pid = MyProcPid;

        if (!RegisterDynamicBackgroundWorker(&worker, &handle))
        {
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            job->status = PGCLONE_JOB_FREE;
            LWLockRelease(pgclone_state->lock);
            ereport(ERROR, (errmsg("pgclone: could not register background worker")));
        }

        /* Wait for the worker to actually start */
        {
            BgwHandleStatus status;
            pid_t           worker_pid;

            status = WaitForBackgroundWorkerStartup(handle, &worker_pid);
            if (status != BGWH_STARTED)
            {
                LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
                job->status = PGCLONE_JOB_FAILED;
                strlcpy(job->error_message, "background worker failed to start", 256);
                job->end_time = GetCurrentTimestamp();
                LWLockRelease(pgclone_state->lock);
                ereport(ERROR,
                        (errmsg("pgclone: background worker failed to start (status=%d)", status)));
            }
        }

        PG_RETURN_INT32(job_id);
    }
}

/* ===============================================================
 * FUNCTION: pgclone_progress(job_id) — returns JSON
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_progress);

Datum
pgclone_progress(PG_FUNCTION_ARGS)
{
    int             job_id = PG_GETARG_INT32(0);
    PgcloneJob    *job;
    StringInfoData  result;
    const char     *status_str;

    if (!pgclone_state)
        ereport(ERROR, (errmsg("pgclone: shared memory not initialized")));

    LWLockAcquire(pgclone_state->lock, LW_SHARED);
    job = find_job(job_id);

    if (!job)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: job %d not found", job_id)));
    }

    switch (job->status)
    {
        case PGCLONE_JOB_PENDING:   status_str = "pending";   break;
        case PGCLONE_JOB_RUNNING:   status_str = "running";   break;
        case PGCLONE_JOB_COMPLETED: status_str = "completed"; break;
        case PGCLONE_JOB_FAILED:    status_str = "failed";    break;
        case PGCLONE_JOB_CANCELLED: status_str = "cancelled"; break;
        default:                      status_str = "unknown";    break;
    }

    initStringInfo(&result);
    appendStringInfo(&result,
        "{\"job_id\": %d, \"status\": \"%s\", \"phase\": \"%s\", "
        "\"tables_completed\": %ld, \"tables_total\": %ld, "
        "\"rows_copied\": %ld, \"current_table\": \"%s\"",
        job->job_id, status_str, job->current_phase,
        (long) job->completed_tables, (long) job->total_tables,
        (long) job->copied_rows, job->current_table);

    if (job->status == PGCLONE_JOB_FAILED)
        appendStringInfo(&result, ", \"error\": \"%s\"", job->error_message);

    if (job->resume_checkpoint[0] != '\0')
        appendStringInfo(&result, ", \"checkpoint\": \"%s\"", job->resume_checkpoint);

    if (job->start_time != 0)
    {
        long elapsed_ms;
        if (job->end_time != 0)
            elapsed_ms = (long)((job->end_time - job->start_time) / 1000);
        else
            elapsed_ms = (long)((GetCurrentTimestamp() - job->start_time) / 1000);
        appendStringInfo(&result, ", \"elapsed_ms\": %ld", elapsed_ms);
    }

    appendStringInfoChar(&result, '}');
    LWLockRelease(pgclone_state->lock);

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/* ===============================================================
 * FUNCTION: pgclone_cancel(job_id)
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_cancel);

Datum
pgclone_cancel(PG_FUNCTION_ARGS)
{
    int             job_id = PG_GETARG_INT32(0);
    PgcloneJob    *job;

    if (!pgclone_state)
        ereport(ERROR, (errmsg("pgclone: shared memory not initialized")));

    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
    job = find_job(job_id);

    if (!job)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: job %d not found", job_id)));
    }

    if (job->status == PGCLONE_JOB_RUNNING ||
        job->status == PGCLONE_JOB_PENDING)
    {
        job->status = PGCLONE_JOB_CANCELLED;
        job->end_time = GetCurrentTimestamp();
        strlcpy(job->current_phase, "cancelled", 64);
    }

    LWLockRelease(pgclone_state->lock);

    PG_RETURN_TEXT_P(cstring_to_text("cancelled"));
}

/* ===============================================================
 * FUNCTION: pgclone_resume(job_id) — returns new job_id
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_resume);

Datum
pgclone_resume(PG_FUNCTION_ARGS)
{
    int             old_job_id = PG_GETARG_INT32(0);
    PgcloneJob    *old_job, *new_job;
    int             new_job_id;
    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;

    if (!pgclone_state)
        ereport(ERROR, (errmsg("pgclone: shared memory not initialized")));

    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

    old_job = find_job(old_job_id);
    if (!old_job)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: job %d not found", old_job_id)));
    }

    if (old_job->status != PGCLONE_JOB_FAILED &&
        old_job->status != PGCLONE_JOB_CANCELLED)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: job %d is not resumable", old_job_id)));
    }

    new_job = find_free_slot();
    if (!new_job)
    {
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: no free job slots")));
    }

    new_job_id = pgclone_state->next_job_id++;
    memcpy(new_job, old_job, sizeof(PgcloneJob));
    new_job->job_id = new_job_id;
    new_job->status = PGCLONE_JOB_PENDING;
    new_job->worker_pid = 0;
    new_job->start_time = 0;
    new_job->end_time = 0;
    new_job->error_message[0] = '\0';
    /* resume_checkpoint preserved — bgworker will skip past it */

    old_job->status = PGCLONE_JOB_FREE;

    LWLockRelease(pgclone_state->lock);

    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "pgclone: resume job %d", new_job_id);
    snprintf(worker.bgw_type, BGW_MAXLEN, "pgclone worker");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgclone");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgclone_bgw_main");
    worker.bgw_main_arg = Int32GetDatum(new_job_id);
    worker.bgw_notify_pid = MyProcPid;

    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
    {
        LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
        new_job->status = PGCLONE_JOB_FREE;
        LWLockRelease(pgclone_state->lock);
        ereport(ERROR, (errmsg("pgclone: could not register background worker")));
    }

    /* Wait for the worker to actually start */
    {
        BgwHandleStatus status;
        pid_t           worker_pid;

        status = WaitForBackgroundWorkerStartup(handle, &worker_pid);
        if (status != BGWH_STARTED)
        {
            LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);
            new_job->status = PGCLONE_JOB_FAILED;
            strlcpy(new_job->error_message, "background worker failed to start", 256);
            new_job->end_time = GetCurrentTimestamp();
            LWLockRelease(pgclone_state->lock);
            ereport(ERROR,
                    (errmsg("pgclone: background worker failed to start (status=%d)", status)));
        }
    }

    PG_RETURN_INT32(new_job_id);
}

/* ===============================================================
 * FUNCTION: pgclone_jobs() — returns JSON array of all jobs
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_jobs);

Datum
pgclone_jobs(PG_FUNCTION_ARGS)
{
    StringInfoData  result;
    int             i;
    bool            first = true;
    const char     *status_str;

    if (!pgclone_state)
        ereport(ERROR, (errmsg("pgclone: shared memory not initialized")));

    initStringInfo(&result);
    appendStringInfoChar(&result, '[');

    LWLockAcquire(pgclone_state->lock, LW_SHARED);

    for (i = 0; i < PGCLONE_MAX_JOBS; i++)
    {
        PgcloneJob *j = &pgclone_state->jobs[i];

        if (j->status == PGCLONE_JOB_FREE)
            continue;

        if (!first)
            appendStringInfoChar(&result, ',');
        first = false;

        switch (j->status)
        {
            case PGCLONE_JOB_PENDING:   status_str = "pending";   break;
            case PGCLONE_JOB_RUNNING:   status_str = "running";   break;
            case PGCLONE_JOB_COMPLETED: status_str = "completed"; break;
            case PGCLONE_JOB_FAILED:    status_str = "failed";    break;
            case PGCLONE_JOB_CANCELLED: status_str = "cancelled"; break;
            default:                      status_str = "unknown";    break;
        }

        appendStringInfo(&result,
            "{\"job_id\": %d, \"status\": \"%s\", \"schema\": \"%s\", "
            "\"table\": \"%s\", \"phase\": \"%s\", "
            "\"tables_completed\": %ld, \"tables_total\": %ld}",
            j->job_id, status_str, j->schema_name,
            j->table_name, j->current_phase,
            (long) j->completed_tables, (long) j->total_tables);
    }

    LWLockRelease(pgclone_state->lock);
    appendStringInfoChar(&result, ']');

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/* ===============================================================
 * FUNCTION: pgclone_clear_jobs() — free completed/failed/cancelled slots
 *
 * Clears all non-active job slots from shared memory so they can
 * be reused. Running and pending jobs are preserved.
 * Returns the number of slots cleared.
 * =============================================================== */
PG_FUNCTION_INFO_V1(pgclone_clear_jobs);

Datum
pgclone_clear_jobs(PG_FUNCTION_ARGS)
{
    int cleared = 0;
    int i;

    if (!pgclone_state)
        ereport(ERROR, (errmsg("pgclone: shared memory not initialized")));

    LWLockAcquire(pgclone_state->lock, LW_EXCLUSIVE);

    for (i = 0; i < PGCLONE_MAX_JOBS; i++)
    {
        PgcloneJob *j = &pgclone_state->jobs[i];

        if (j->status == PGCLONE_JOB_COMPLETED ||
            j->status == PGCLONE_JOB_FAILED ||
            j->status == PGCLONE_JOB_CANCELLED)
        {
            j->status = PGCLONE_JOB_FREE;
            cleared++;
        }
    }

    LWLockRelease(pgclone_state->lock);

    ereport(NOTICE,
            (errmsg("pgclone: cleared %d finished job slots", cleared)));

    PG_RETURN_INT32(cleared);
}

/* ===============================================================
 * FUNCTION: pgclone_progress_view() — SET-RETURNING FUNCTION
 *
 * Returns one row per active/recent job from shared memory.
 * Designed to back the pgclone_jobs_view VIEW.
 *
 * Columns:
 *   job_id              INTEGER
 *   status              TEXT
 *   op_type             TEXT
 *   schema_name         TEXT
 *   table_name          TEXT
 *   current_phase       TEXT
 *   current_table       TEXT
 *   tables_total        BIGINT
 *   tables_completed    BIGINT
 *   rows_copied         BIGINT
 *   bytes_copied        BIGINT
 *   elapsed_ms          BIGINT
 *   start_time          TIMESTAMPTZ
 *   end_time            TIMESTAMPTZ
 *   error_message       TEXT
 *   pct_complete        DOUBLE PRECISION
 *   progress_bar        TEXT
 *   elapsed_time        TEXT        (human-readable: HH:MM:SS)
 * =============================================================== */
#define PGCLONE_VIEW_COLS 18

PG_FUNCTION_INFO_V1(pgclone_progress_view);

Datum
pgclone_progress_view(PG_FUNCTION_ARGS)
{
    FuncCallContext    *funcctx;
    PgcloneJob         *job;
    int                 slot_index;

    if (!pgclone_state)
        ereport(ERROR, (errmsg("pgclone: shared memory not initialized — "
                               "add pgclone to shared_preload_libraries")));

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext   oldctx;
        TupleDesc       tupdesc;

        funcctx = SRF_FIRSTCALL_INIT();
        oldctx  = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        tupdesc = CreateTemplateTupleDesc(PGCLONE_VIEW_COLS);
        TupleDescInitEntry(tupdesc,  1, "job_id",           INT4OID,   -1, 0);
        TupleDescInitEntry(tupdesc,  2, "status",           TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  3, "op_type",          TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  4, "schema_name",      TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  5, "table_name",       TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  6, "current_phase",    TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  7, "current_table",    TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc,  8, "tables_total",     INT8OID,   -1, 0);
        TupleDescInitEntry(tupdesc,  9, "tables_completed", INT8OID,   -1, 0);
        TupleDescInitEntry(tupdesc, 10, "rows_copied",      INT8OID,   -1, 0);
        TupleDescInitEntry(tupdesc, 11, "bytes_copied",     INT8OID,   -1, 0);
        TupleDescInitEntry(tupdesc, 12, "elapsed_ms",       INT8OID,   -1, 0);
        TupleDescInitEntry(tupdesc, 13, "start_time",       TIMESTAMPTZOID, -1, 0);
        TupleDescInitEntry(tupdesc, 14, "end_time",         TIMESTAMPTZOID, -1, 0);
        TupleDescInitEntry(tupdesc, 15, "error_message",    TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc, 16, "pct_complete",     FLOAT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 17, "progress_bar",         TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc, 18, "elapsed_time",         TEXTOID,   -1, 0);

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        funcctx->user_fctx  = (void *)(intptr_t) 0;   /* slot_index */

        MemoryContextSwitchTo(oldctx);
    }

    funcctx    = SRF_PERCALL_SETUP();
    slot_index = (int)(intptr_t) funcctx->user_fctx;

    /* Scan shared memory slots for next non-free job */
    while (slot_index < PGCLONE_MAX_JOBS)
    {
        Datum       values[PGCLONE_VIEW_COLS];
        bool        nulls[PGCLONE_VIEW_COLS];
        HeapTuple   tuple;
        const char *status_str;
        const char *op_str;
        int64       elapsed_ms = 0;
        float8      pct = 0.0;

        LWLockAcquire(pgclone_state->lock, LW_SHARED);
        job = &pgclone_state->jobs[slot_index];

        if (job->status == PGCLONE_JOB_FREE)
        {
            LWLockRelease(pgclone_state->lock);
            slot_index++;
            continue;
        }

        /* Build the row while holding the lock */
        memset(nulls, 0, sizeof(nulls));

        /* status string */
        switch (job->status)
        {
            case PGCLONE_JOB_PENDING:   status_str = "pending";   break;
            case PGCLONE_JOB_RUNNING:   status_str = "running";   break;
            case PGCLONE_JOB_COMPLETED: status_str = "completed"; break;
            case PGCLONE_JOB_FAILED:    status_str = "failed";    break;
            case PGCLONE_JOB_CANCELLED: status_str = "cancelled"; break;
            default:                    status_str = "unknown";    break;
        }

        /* op_type string */
        switch (job->op_type)
        {
            case PGCLONE_OP_TABLE:    op_str = "table";    break;
            case PGCLONE_OP_SCHEMA:   op_str = "schema";   break;
            case PGCLONE_OP_DATABASE: op_str = "database"; break;
            default:                  op_str = "unknown";   break;
        }

        /* elapsed */
        if (job->start_time != 0)
        {
            if (job->end_time != 0)
                elapsed_ms = (job->end_time - job->start_time) / 1000;
            else
                elapsed_ms = (GetCurrentTimestamp() - job->start_time) / 1000;
        }

        /* percentage */
        if (job->total_tables > 0)
            pct = (float8) job->completed_tables / (float8) job->total_tables * 100.0;

        /* Fill datum array */
        values[0]  = Int32GetDatum(job->job_id);
        values[1]  = CStringGetTextDatum(status_str);
        values[2]  = CStringGetTextDatum(op_str);
        values[3]  = CStringGetTextDatum(job->schema_name);
        values[4]  = CStringGetTextDatum(job->table_name);
        values[5]  = CStringGetTextDatum(job->current_phase);
        values[6]  = CStringGetTextDatum(job->current_table);
        values[7]  = Int64GetDatum(job->total_tables);
        values[8]  = Int64GetDatum(job->completed_tables);
        values[9]  = Int64GetDatum(job->copied_rows);
        values[10] = Int64GetDatum(job->copied_bytes);
        values[11] = Int64GetDatum(elapsed_ms);

        if (job->start_time != 0)
            values[12] = TimestampTzGetDatum(job->start_time);
        else
            nulls[12] = true;

        if (job->end_time != 0)
            values[13] = TimestampTzGetDatum(job->end_time);
        else
            nulls[13] = true;

        if (job->error_message[0] != '\0')
            values[14] = CStringGetTextDatum(job->error_message);
        else
            nulls[14] = true;

        values[15] = Float8GetDatum(pct);

        /* Build progress bar with elapsed time */
        {
            char        bar[128];
            int         filled;
            int         empty;
            int         bi;
            int         pi;
            const int   bar_width = 20;
            long        elapsed_sec;
            char        elapsed_str[32];

            filled = (int)(pct / 100.0 * bar_width);
            if (filled > bar_width) filled = bar_width;
            empty = bar_width - filled;

            bi = 0;
            bar[bi++] = '[';
            for (pi = 0; pi < filled; pi++)
            {
                /* UTF-8 for █ (U+2588): 0xE2 0x96 0x88 */
                bar[bi++] = (char)0xE2;
                bar[bi++] = (char)0x96;
                bar[bi++] = (char)0x88;
            }
            for (pi = 0; pi < empty; pi++)
            {
                /* UTF-8 for ░ (U+2591): 0xE2 0x96 0x91 */
                bar[bi++] = (char)0xE2;
                bar[bi++] = (char)0x96;
                bar[bi++] = (char)0x91;
            }
            bar[bi++] = ']';
            bar[bi] = '\0';

            /* Format elapsed time as HH:MM:SS */
            elapsed_sec = elapsed_ms / 1000;
            snprintf(elapsed_str, sizeof(elapsed_str), "%02ld:%02ld:%02ld",
                     elapsed_sec / 3600,
                     (elapsed_sec % 3600) / 60,
                     elapsed_sec % 60);

            /* progress_bar column */
            {
                char full_bar[512];
                snprintf(full_bar, sizeof(full_bar),
                         "%s %.1f%% | %ld rows | %s elapsed",
                         bar, pct, (long) job->copied_rows,
                         elapsed_str);
                values[16] = CStringGetTextDatum(full_bar);
            }

            /* elapsed_time column (HH:MM:SS) */
            values[17] = CStringGetTextDatum(elapsed_str);
        }

        LWLockRelease(pgclone_state->lock);

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

        /* Advance to next slot for the next call */
        funcctx->user_fctx = (void *)(intptr_t)(slot_index + 1);

        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    SRF_RETURN_DONE(funcctx);
}
