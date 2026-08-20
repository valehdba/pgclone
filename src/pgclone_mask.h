/*
 * pgclone_mask.h - column masking types and helpers.
 *
 * Extracted from pgclone.c so mask rules, mask expressions, and
 * type/length/constraint compatibility checks live in one file.
 * Included by pgclone.c (COPY orchestrator + discover / mask_in_place
 * / masking_policy / masking_report entry points) and any future
 * async-side worker that gains masking support.
 *
 * The helpers deliberately do not take a full CloneOptions*; instead
 * callers pass the array-of-rules directly. That keeps this header
 * free of the CloneOptions dependency so pgclone_mask.o does not
 * couple to every other subsystem.
 */

#ifndef PGCLONE_MASK_H
#define PGCLONE_MASK_H

#include "postgres.h"
#include "lib/stringinfo.h"
#include "libpq-fe.h"

#define PGCLONE_MAX_MASKS          64
#define PGCLONE_MAX_TABLE_MASKS    64

/* Mask strategy types. Keep the enum stable — the ordinal values are
 * embedded in options-parsing switch statements only, not in on-wire
 * or on-disk formats, so appending new variants is safe. */
typedef enum PgcloneMaskType
{
    PGCLONE_MASK_NONE = 0,
    PGCLONE_MASK_EMAIL,         /* a***@domain.com */
    PGCLONE_MASK_NAME,          /* XXXX */
    PGCLONE_MASK_PHONE,         /* ***-***-1234 */
    PGCLONE_MASK_PARTIAL,       /* Jo***on — keep first/last N chars */
    PGCLONE_MASK_HASH,          /* MD5 — deterministic for referential integrity */
    PGCLONE_MASK_NULL,          /* replace with NULL */
    PGCLONE_MASK_RANDOM_INT,    /* random integer in [min, max] */
    PGCLONE_MASK_CONSTANT       /* fixed replacement value */
} PgcloneMaskType;

/* Per-column masking rule. Populated by the JSON options parser. */
typedef struct MaskRule
{
    char              column[NAMEDATALEN];
    PgcloneMaskType   type;
    int               partial_prefix;    /* PARTIAL: chars to keep at start */
    int               partial_suffix;    /* PARTIAL: chars to keep at end   */
    int               range_min;         /* RANDOM_INT: min value           */
    int               range_max;         /* RANDOM_INT: max value           */
    char              constant_val[256]; /* CONSTANT: replacement literal   */
} MaskRule;

/* v4.4.1 (issue #17). Kind of value a mask strategy emits, so a
 * caller can decide up-front whether that value can be stored in
 * the target column's type. */
typedef enum PgcloneMaskOutKind
{
    MASK_OUT_ASIS = 0,   /* column emitted unchanged (no mask)          */
    MASK_OUT_TEXT,       /* text value: email/name/phone/partial/hash   */
    MASK_OUT_NUMERIC,    /* integer value: random_int                   */
    MASK_OUT_NULLONLY,   /* SQL NULL: null                              */
    MASK_OUT_ANY         /* caller-supplied literal: constant (trusted) */
} PgcloneMaskOutKind;

/* v4.4.2 (issue #18). Per-column facts consulted by the constraint-
 * and length-aware skip logic. Field values '\0' / 0 / false mean
 * "unknown, be permissive". */
typedef struct ColMaskMeta
{
    char  typcat;        /* pg_type.typcategory, or '\0' if unknown        */
    int   char_maxlen;   /* declared varchar/char length, 0 = unlimited    */
    bool  notnull;       /* column is NOT NULL                             */
    bool  is_unique;     /* participates in a PK / UNIQUE index            */
    bool  is_fk;         /* participates in a FOREIGN KEY constraint       */
} ColMaskMeta;

/* Shared SQL fragment that computes ColMaskMeta columns in a fixed
 * order (typcategory, char_maxlen, notnull, is_unique, is_fk).
 * `a`, `c`, `t` must be bound to pg_attribute / pg_class / pg_type
 * in the enclosing query. */
#define PGCLONE_MASKMETA_COLS \
    "t.typcategory, " \
    "CASE WHEN a.atttypmod > 4 AND t.typcategory = 'S' " \
    "     THEN a.atttypmod - 4 ELSE 0 END, " \
    "a.attnotnull, " \
    "EXISTS (SELECT 1 FROM pg_catalog.pg_index i " \
    "        WHERE i.indrelid = c.oid AND i.indisunique " \
    "          AND a.attnum = ANY(i.indkey)), " \
    "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint con " \
    "        WHERE con.conrelid = c.oid AND con.contype = 'f' " \
    "          AND a.attnum = ANY(con.conkey))"

/* Emit the raw SQL expression that computes the masked value for
 * col_ident under `rule`. No length clamp — see the _clamped variant. */
extern void pgclone_build_mask_expr(StringInfo buf,
                                    const char *col_ident,
                                    const MaskRule *rule);

/* Emit the SQL expression clamped to `char_maxlen` chars via left(...);
 * char_maxlen == 0 means unlimited. Handles issue #18 length overflow. */
extern void pgclone_append_mask_expr_clamped(StringInfo out,
                                             const char *col_ident,
                                             const MaskRule *rule,
                                             int char_maxlen);

/* Linear search over `masks` for a rule matching col_name; NULL if none. */
extern const MaskRule *pgclone_find_mask_rule(const MaskRule *masks,
                                              int num_masks,
                                              const char *col_name);

/* Look up the raw JSON string for a per-table mask (v4.4.0 "masks"),
 * preferring schema-qualified keys ("schema.table") over bare table
 * names. Returns NULL if no entry matches. */
extern const char *pgclone_find_table_mask(const char *const *keys,
                                           const char *const *jsons,
                                           int num_table_masks,
                                           const char *schema_name,
                                           const char *table_name);

/* Which kind of value does a strategy emit? */
extern PgcloneMaskOutKind pgclone_mask_out_kind(PgcloneMaskType t);

/* Can a value of `kind` be stored in a column with `typcat`? */
extern bool pgclone_mask_kind_fits(PgcloneMaskOutKind kind, char typcat);

/* Same question keyed by a strategy string (discover_sensitive path). */
extern bool pgclone_strategy_fits(const char *strategy, char typcat);

/* Human-readable strategy name / typcategory label, for WARNING text. */
extern const char *pgclone_masktype_name(PgcloneMaskType t);
extern const char *pgclone_typcat_desc(char typcat);

/* True when `s` parses cleanly as a number. */
extern bool pgclone_looks_numeric(const char *s);

/* Populate ColMaskMeta from a result row starting at column `base`
 * (columns must be laid out per PGCLONE_MASKMETA_COLS). */
extern ColMaskMeta pgclone_maskmeta_from_row(PGresult *r, int row, int base);

/* Look up ColMaskMeta for one schema.table.column. On any failure
 * returns a zeroed struct (typcat '\0'), which the decision logic
 * treats permissively. */
extern ColMaskMeta pgclone_column_maskmeta(PGconn *conn,
                                           const char *schema_name,
                                           const char *table_name,
                                           const char *col_name);

/* Consolidated skip logic — NULL to apply, or a short human reason.
 * Covers type mismatch, invalid constant literal, NULL-into-NOT-NULL,
 * FK columns, and non-injective masks on UNIQUE/PK. */
extern const char *pgclone_mask_skip_reason(const MaskRule *rule,
                                            const ColMaskMeta *m);

/* Adjust a discover_sensitive strategy suggestion so the engine will
 * actually apply it: drops FK columns, promotes UNIQUE/PK and NULL-on-
 * NOT-NULL suggestions to "hash", and drops incompatible-type outputs.
 * Returns NULL to omit the column. */
extern const char *pgclone_discover_strategy(const char *base,
                                             const ColMaskMeta *m);

#endif /* PGCLONE_MASK_H */
