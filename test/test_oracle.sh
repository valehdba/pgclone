#!/bin/bash
# ============================================================
# Integration test for pgclone.table_from_oracle().
# Needs: pgclone built with WITH_ORACLE=1, an Oracle instance seeded with
# test/fixtures/oracle_seed.sql, and a target PostgreSQL (PGHOST/PGPORT/...).
#
#   ORACLE_CONNINFO="host=localhost port=1521 service=FREEPDB1 user=app password=apppass" \
#   PGHOST=localhost PGPORT=5434 PGUSER=postgres bash test/test_oracle.sh
# ============================================================
set -euo pipefail

: "${ORACLE_CONNINFO:?set ORACLE_CONNINFO}"
PGDATABASE="${PGDATABASE:-target_db}"
export PGDATABASE
export PGCLIENTENCODING=UTF8
ORA_SCHEMA="${ORA_SCHEMA:-APP}"

PASS=0; FAIL=0
q() { psql -X -v ON_ERROR_STOP=1 -tA -c "$1"; }
check() {   # check "description" "expected" "actual"
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1 (expected [$2], got [$3])"; FAIL=$((FAIL + 1)); fi
}

q "DROP TABLE IF EXISTS public.pgc_types, public.pgc_big, public.pgc_empty, public.pgc_nodata" >/dev/null
q "CREATE EXTENSION IF NOT EXISTS pgclone" >/dev/null

echo "---- dry run ----"
DDL=$(q "SELECT pgclone.table_from_oracle('$ORACLE_CONNINFO', '$ORA_SCHEMA', 'PGC_TYPES', dry_run => true)" 2>/dev/null)
echo "$DDL" | grep -q "CREATE TABLE public.pgc_types" && R=ok || R=missing
check "dry_run returns CREATE TABLE" ok "$R"
echo "$DDL" | grep -q "PRIMARY KEY (id)" && R=ok || R=missing
check "dry_run includes PRIMARY KEY" ok "$R"
echo "$DDL" | grep -q "UNIQUE (email)" && R=ok || R=missing
check "dry_run includes UNIQUE" ok "$R"
echo "$DDL" | grep -q "blob_c" && R=present || R=absent
check "unsupported BLOB column skipped" absent "$R"
check "dry_run creates nothing" 0 "$(q "SELECT count(*) FROM pg_tables WHERE tablename = 'pgc_types'")"

echo "---- clone with data (lower-case names accepted) ----"
q "SELECT pgclone.table_from_oracle('$ORACLE_CONNINFO', '$ORA_SCHEMA', 'pgc_types')" >/dev/null 2>&1
check "row count" 3 "$(q "SELECT count(*) FROM public.pgc_types")"

echo "---- column types ----"
ty() { q "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'public.pgc_types'::regclass AND attname = '$1'"; }
check "NUMBER(9) -> integer"        integer                      "$(ty id)"
check "NUMBER(18) -> bigint"        bigint                       "$(ty big_n)"
check "NUMBER(10,2) -> numeric"     "numeric(10,2)"              "$(ty amt)"
check "NUMBER -> numeric"           numeric                      "$(ty any_n)"
check "BINARY_DOUBLE"               "double precision"           "$(ty dbl)"
check "VARCHAR2(50 CHAR)"           "character varying(50)"      "$(ty name)"
check "CHAR(3)"                     "character(3)"               "$(ty code)"
check "CLOB -> text"                text                         "$(ty notes)"
check "DATE -> timestamp(0)"        "timestamp(0) without time zone" "$(ty d)"
check "TIMESTAMP(6)"                "timestamp(6) without time zone" "$(ty ts)"
check "TIMESTAMP WITH TIME ZONE"    "timestamp(6) with time zone"    "$(ty tstz)"
check "RAW -> bytea"                bytea                        "$(ty raw_c)"
check "PK present"                  1 "$(q "SELECT count(*) FROM pg_index WHERE indrelid = 'public.pgc_types'::regclass AND indisprimary")"
check "UNIQUE present"              1 "$(q "SELECT count(*) FROM pg_index WHERE indrelid = 'public.pgc_types'::regclass AND indisunique AND NOT indisprimary")"
check "NOT NULL kept"               t "$(q "SELECT attnotnull FROM pg_attribute WHERE attrelid = 'public.pgc_types'::regclass AND attname = 'name'")"

echo "---- values ----"
v() { q "SELECT $1 FROM public.pgc_types WHERE id = $2"; }
check "numeric(10,2)"          12345.67                         "$(v amt 1)"
check "bigint"                 123456789012345678               "$(v big_n 1)"
check "binary_double"          2.5                              "$(v dbl 1)"
check "unicode + tab"          "Əli Vəliyev	tab"                "$(v name 1)"
check "CHAR padding"           ABC                              "$(v code 1)"
check "CLOB length (100000)"   100000                           "$(v 'length(notes)' 1)"
check "DATE keeps time"        "2024-02-29 13:14:15"            "$(v d 1)"
check "TIMESTAMP micros"       "2024-01-02 03:04:05.123456"     "$(v ts 1)"
check "TIMESTAMPTZ (UTC)"      "2024-01-02 01:04:05.5+00"       "$(q "SET timezone = 'UTC'; SELECT tstz FROM public.pgc_types WHERE id = 1" | tail -1)"
check "RAW hex"                deadbeef                         "$(v "encode(raw_c, 'hex')" 1)"
check "all-NULL row"           t "$(q "SELECT small_n IS NULL AND amt IS NULL AND notes IS NULL AND d IS NULL AND tstz IS NULL AND raw_c IS NULL FROM public.pgc_types WHERE id = 2")"
check "backslash + newline"    t "$(q "SELECT name = E'back\\\\slash\\nnewline' FROM public.pgc_types WHERE id = 3")"

echo "---- larger table (multiple COPY flushes) ----"
q "SELECT pgclone.table_from_oracle('$ORACLE_CONNINFO', '$ORA_SCHEMA', 'PGC_BIG')" >/dev/null 2>&1
check "20000 rows"  20000 "$(q "SELECT count(*) FROM public.pgc_big")"
check "sum(id)"     200010000 "$(q "SELECT sum(id) FROM public.pgc_big")"
check "last label"  "row number 20000" "$(q "SELECT label FROM public.pgc_big WHERE id = 20000")"

echo "---- options and failure modes ----"
q "SELECT pgclone.table_from_oracle('$ORACLE_CONNINFO', '$ORA_SCHEMA', 'PGC_BIG', include_data => false, target_table => 'pgc_nodata')" >/dev/null 2>&1
check "include_data=false -> empty table" 0 "$(q "SELECT count(*) FROM public.pgc_nodata")"

if q "SELECT pgclone.table_from_oracle('$ORACLE_CONNINFO', '$ORA_SCHEMA', 'PGC_BIG')" >/dev/null 2>&1; then R=succeeded; else R=failed; fi
check "existing target is never overwritten" failed "$R"
check "existing target data untouched" 20000 "$(q "SELECT count(*) FROM public.pgc_big")"

ERR=$(q "SELECT pgclone.table_from_oracle('$ORACLE_CONNINFO', '$ORA_SCHEMA', 'NO_SUCH_TABLE')" 2>&1 || true)
echo "$ERR" | grep -q "not found" && R=reported || R="no message: $ERR"
check "missing Oracle table reported" reported "$R"
check "no leftover table after failure" 0 "$(q "SELECT count(*) FROM pg_tables WHERE tablename = 'no_such_table'")"

BAD="${ORACLE_CONNINFO/password=*/password=wrong}"
if q "SELECT pgclone.table_from_oracle('$BAD', '$ORA_SCHEMA', 'PGC_BIG', target_table => 'pgc_empty')" >/dev/null 2>&1; then R=succeeded; else R=failed; fi
check "bad password fails" failed "$R"
check "no table after failed login" 0 "$(q "SELECT count(*) FROM pg_tables WHERE tablename = 'pgc_empty'")"

echo ""
echo "ORACLE: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
