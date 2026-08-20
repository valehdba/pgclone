/*
 * pgclone_mask.c - column masking implementation.
 *
 * Extracted from pgclone.c (v4.4.2 → structural cleanup). See
 * pgclone_mask.h for the public API contract; all functions here
 * are declared there.
 *
 * Every mask expression is emitted server-side, so masking happens
 * inside COPY (SELECT ...) TO STDOUT on the source and unmasked
 * rows never enter the local database. The helpers below only
 * build SQL fragments — they do not run queries themselves
 * (with the exception of pgclone_column_maskmeta, which needs to
 * query pg_attribute/pg_class/pg_type on the source).
 */

#include "postgres.h"
#include "utils/builtins.h"
#include "lib/stringinfo.h"
#include "libpq-fe.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "pgclone_mask.h"

void
pgclone_build_mask_expr(StringInfo buf, const char *col_ident,
                        const MaskRule *rule)
{
    switch (rule->type)
    {
        case PGCLONE_MASK_EMAIL:
            /* "alice@example.com" -> "a***@example.com" (NULL-safe). */
            appendStringInfo(buf,
                "CASE WHEN %s IS NULL THEN NULL "
                "WHEN position('@' in %s::text) > 0 THEN "
                "  left(%s::text, 1) || '***@' || split_part(%s::text, '@', 2) "
                "ELSE '***' END",
                col_ident, col_ident, col_ident, col_ident);
            break;

        case PGCLONE_MASK_NAME:
            appendStringInfo(buf,
                "CASE WHEN %s IS NULL THEN NULL ELSE 'XXXX' END",
                col_ident);
            break;

        case PGCLONE_MASK_PHONE:
            /* "+1-555-123-4567" -> "***-4567" */
            appendStringInfo(buf,
                "CASE WHEN %s IS NULL THEN NULL "
                "WHEN length(%s::text) > 4 THEN '***-' || right(%s::text, 4) "
                "ELSE '****' END",
                col_ident, col_ident, col_ident);
            break;

        case PGCLONE_MASK_PARTIAL:
            /* prefix=2, suffix=3: "Johnson" -> "Jo***son" */
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
            /* Deterministic MD5 — preserves referential integrity across
             * tables when the same source value maps to the same hash. */
            appendStringInfo(buf,
                "CASE WHEN %s IS NULL THEN NULL "
                "ELSE md5(%s::text) END",
                col_ident, col_ident);
            break;

        case PGCLONE_MASK_NULL:
            appendStringInfoString(buf, "NULL");
            break;

        case PGCLONE_MASK_RANDOM_INT:
            {
                int rmin = rule->range_min;
                int rmax = rule->range_max;

                if (rmin == 0 && rmax == 0)
                    rmax = 99999;
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
            appendStringInfoString(buf, col_ident);
            break;
    }
}

const MaskRule *
pgclone_find_mask_rule(const MaskRule *masks, int num_masks,
                       const char *col_name)
{
    int i;

    if (masks == NULL || num_masks == 0)
        return NULL;

    for (i = 0; i < num_masks; i++)
    {
        if (strcmp(masks[i].column, col_name) == 0)
            return &masks[i];
    }
    return NULL;
}

PgcloneMaskOutKind
pgclone_mask_out_kind(PgcloneMaskType t)
{
    switch (t)
    {
        case PGCLONE_MASK_EMAIL:
        case PGCLONE_MASK_NAME:
        case PGCLONE_MASK_PHONE:
        case PGCLONE_MASK_PARTIAL:
        case PGCLONE_MASK_HASH:
            return MASK_OUT_TEXT;
        case PGCLONE_MASK_RANDOM_INT:
            return MASK_OUT_NUMERIC;
        case PGCLONE_MASK_NULL:
            return MASK_OUT_NULLONLY;
        case PGCLONE_MASK_CONSTANT:
            return MASK_OUT_ANY;
        case PGCLONE_MASK_NONE:
        default:
            return MASK_OUT_ASIS;
    }
}

bool
pgclone_mask_kind_fits(PgcloneMaskOutKind kind, char typcat)
{
    if (typcat == '\0')
        return true;

    switch (kind)
    {
        case MASK_OUT_TEXT:
            return typcat == 'S';
        case MASK_OUT_NUMERIC:
            return typcat == 'N' || typcat == 'S';
        case MASK_OUT_NULLONLY:
        case MASK_OUT_ANY:
        case MASK_OUT_ASIS:
        default:
            return true;
    }
}

bool
pgclone_strategy_fits(const char *strategy, char typcat)
{
    PgcloneMaskOutKind kind;

    if (strategy == NULL)
        return true;
    else if (strcmp(strategy, "random_int") == 0)
        kind = MASK_OUT_NUMERIC;
    else if (strcmp(strategy, "null") == 0)
        kind = MASK_OUT_NULLONLY;
    else
        kind = MASK_OUT_TEXT;   /* email/name/phone/partial/hash/constant */

    return pgclone_mask_kind_fits(kind, typcat);
}

const char *
pgclone_masktype_name(PgcloneMaskType t)
{
    switch (t)
    {
        case PGCLONE_MASK_EMAIL:      return "email";
        case PGCLONE_MASK_NAME:       return "name";
        case PGCLONE_MASK_PHONE:      return "phone";
        case PGCLONE_MASK_PARTIAL:    return "partial";
        case PGCLONE_MASK_HASH:       return "hash";
        case PGCLONE_MASK_NULL:       return "null";
        case PGCLONE_MASK_RANDOM_INT: return "random_int";
        case PGCLONE_MASK_CONSTANT:   return "constant";
        case PGCLONE_MASK_NONE:
        default:                      return "none";
    }
}

const char *
pgclone_typcat_desc(char typcat)
{
    switch (typcat)
    {
        case 'B': return "boolean";
        case 'D': return "date/time";
        case 'T': return "timespan";
        case 'N': return "numeric";
        case 'S': return "string";
        case 'E': return "enum";
        case 'G': return "geometric";
        case 'I': return "network address";
        case 'R': return "range";
        case 'A': return "array";
        case 'U': return "binary/user-defined";
        default:  return "this";
    }
}

bool
pgclone_looks_numeric(const char *s)
{
    char *end;

    if (s == NULL || *s == '\0')
        return false;
    errno = 0;
    (void) strtod(s, &end);
    if (errno != 0 || end == s)
        return false;
    while (*end == ' ' || *end == '\t')
        end++;
    return *end == '\0';
}

ColMaskMeta
pgclone_maskmeta_from_row(PGresult *r, int row, int base)
{
    ColMaskMeta m;
    char       *v;

    memset(&m, 0, sizeof(m));
    v = PQgetvalue(r, row, base + 0);
    if (v != NULL && v[0] != '\0')
        m.typcat = v[0];
    m.char_maxlen = atoi(PQgetvalue(r, row, base + 1));
    m.notnull   = (PQgetvalue(r, row, base + 2)[0] == 't');
    m.is_unique = (PQgetvalue(r, row, base + 3)[0] == 't');
    m.is_fk     = (PQgetvalue(r, row, base + 4)[0] == 't');
    return m;
}

ColMaskMeta
pgclone_column_maskmeta(PGconn *conn, const char *schema_name,
                        const char *table_name, const char *col_name)
{
    ColMaskMeta     meta;
    StringInfoData  q;
    PGresult       *r;

    memset(&meta, 0, sizeof(meta));

    initStringInfo(&q);
    appendStringInfo(&q,
        "SELECT " PGCLONE_MASKMETA_COLS " "
        "FROM pg_catalog.pg_attribute a "
        "JOIN pg_catalog.pg_class c ON c.oid = a.attrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "JOIN pg_catalog.pg_type t ON t.oid = a.atttypid "
        "WHERE n.nspname = %s AND c.relname = %s AND a.attname = %s "
        "AND a.attnum > 0 AND NOT a.attisdropped",
        quote_literal_cstr(schema_name),
        quote_literal_cstr(table_name),
        quote_literal_cstr(col_name));

    r = PQexec(conn, q.data);
    pfree(q.data);

    if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0)
        meta = pgclone_maskmeta_from_row(r, 0, 0);
    PQclear(r);
    return meta;
}

const char *
pgclone_mask_skip_reason(const MaskRule *rule, const ColMaskMeta *m)
{
    PgcloneMaskOutKind kind = pgclone_mask_out_kind(rule->type);

    if (!pgclone_mask_kind_fits(kind, m->typcat))
        return "output type is incompatible with the column type";

    if (rule->type == PGCLONE_MASK_CONSTANT &&
        m->typcat != 'S' && m->typcat != '\0')
    {
        if (!(m->typcat == 'N' && pgclone_looks_numeric(rule->constant_val)))
            return "constant value is not valid for the column type";
    }

    if (rule->type == PGCLONE_MASK_NULL && m->notnull)
        return "would put NULL in a NOT NULL column";

    if (m->is_fk)
        return "column is a foreign key (masking would break referential integrity)";

    if (m->is_unique && rule->type != PGCLONE_MASK_HASH)
        return "column is UNIQUE/PRIMARY KEY (only the \"hash\" strategy preserves uniqueness)";

    return NULL;
}

const char *
pgclone_discover_strategy(const char *base, const ColMaskMeta *m)
{
    const char *strat = base;

    if (m->is_fk)
        return NULL;

    if ((m->is_unique && strcmp(strat, "hash") != 0) ||
        (strcmp(strat, "null") == 0 && m->notnull))
        strat = "hash";

    if (!pgclone_strategy_fits(strat, m->typcat))
        return NULL;

    return strat;
}

void
pgclone_append_mask_expr_clamped(StringInfo out, const char *col_ident,
                                 const MaskRule *rule, int char_maxlen)
{
    if (char_maxlen > 0)
    {
        appendStringInfoString(out, "left((");
        pgclone_build_mask_expr(out, col_ident, rule);
        appendStringInfo(out, ")::text, %d)", char_maxlen);
    }
    else
        pgclone_build_mask_expr(out, col_ident, rule);
}

const char *
pgclone_find_table_mask(const char *const *keys, const char *const *jsons,
                        int num_table_masks,
                        const char *schema_name, const char *table_name)
{
    int   i;
    char *qualified;

    if (keys == NULL || num_table_masks == 0)
        return NULL;

    qualified = psprintf("%s.%s", schema_name, table_name);
    for (i = 0; i < num_table_masks; i++)
    {
        if (strcmp(keys[i], qualified) == 0)
        {
            pfree(qualified);
            return jsons[i];
        }
    }
    pfree(qualified);

    for (i = 0; i < num_table_masks; i++)
    {
        if (strcmp(keys[i], table_name) == 0)
            return jsons[i];
    }
    return NULL;
}
