#!/bin/bash
# ============================================================
# Integration test for pgclone.table_from_sqlserver().
# Needs: pgclone built with WITH_SQLSERVER=1, a SQL Server instance seeded with
# test/fixtures/sqlserver_seed.sql, and a target PostgreSQL (PGHOST/PGPORT/...).
#
#   SQLSERVER_CONNINFO="host=localhost port=1433 database=pgc_test user=sa password=..." \
#   PGHOST=localhost PGPORT=5434 PGUSER=postgres bash test/test_sqlserver.sh
# ============================================================
set -euo pipefail

: "${SQLSERVER_CONNINFO:?set SQLSERVER_CONNINFO}"
PGDATABASE="${PGDATABASE:-target_db}"
export PGDATABASE
export PGCLIENTENCODING=UTF8
SS_SCHEMA="${SS_SCHEMA:-dbo}"

PASS=0; FAIL=0
q() { psql -X -v ON_ERROR_STOP=1 -tA -c "$1"; }
check() {   # check "description" "expected" "actual"
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1 (expected [$2], got [$3])"; FAIL=$((FAIL + 1)); fi
}

q "DROP TABLE IF EXISTS public.pgc_types, public.pgc_big, public.pgc_empty, public.pgc_nodata" >/dev/null
q "CREATE EXTENSION IF NOT EXISTS pgclone" >/dev/null

echo "---- dry run ----"
DDL=$(q "SELECT pgclone.table_from_sqlserver('$SQLSERVER_CONNINFO', '$SS_SCHEMA', 'pgc_types', dry_run => true)" 2>/dev/null)
echo "$DDL" | grep -q "CREATE TABLE public.pgc_types" && R=ok || R=missing
check "dry_run returns CREATE TABLE" ok "$R"
echo "$DDL" | grep -q "PRIMARY KEY (id)" && R=ok || R=missing
check "dry_run includes PRIMARY KEY" ok "$R"
echo "$DDL" | grep -q "UNIQUE (email)" && R=ok || R=missing
check "dry_run includes UNIQUE" ok "$R"
echo "$DDL" | grep -q "geo" && R=present || R=absent
check "unsupported GEOGRAPHY column skipped" absent "$R"
check "dry_run creates nothing" 0 "$(q "SELECT count(*) FROM pg_tables WHERE tablename = 'pgc_types'")"

echo "---- clone with data ----"
q "SELECT pgclone.table_from_sqlserver('$SQLSERVER_CONNINFO', '$SS_SCHEMA', 'pgc_types')" >/dev/null 2>&1
check "row count" 3 "$(q "SELECT count(*) FROM public.pgc_types")"

echo "---- column types ----"
ty() { q "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'public.pgc_types'::regclass AND attname = '$1'"; }
check "INT -> integer"              integer                      "$(ty id)"
check "SMALLINT"                    smallint                     "$(ty small_n)"
check "TINYINT -> smallint"         smallint                     "$(ty tiny_n)"
check "BIGINT"                      bigint                       "$(ty big_n)"
check "BIT -> boolean"              boolean                      "$(ty flag)"
check "DECIMAL(10,2) -> numeric"    "numeric(10,2)"              "$(ty amt)"
check "MONEY -> numeric(19,4)"      "numeric(19,4)"              "$(ty money_c)"
check "REAL"                        real                         "$(ty rl)"
check "FLOAT"                       "double precision"           "$(ty dbl)"
check "NVARCHAR(50)"                "character varying(50)"      "$(ty name)"
check "CHAR(3)"                     "character(3)"               "$(ty code)"
check "NVARCHAR(MAX) -> text"       text                         "$(ty notes)"
check "DATE"                        date                         "$(ty d)"
check "TIME(3)"                     "time(3) without time zone"  "$(ty t)"
check "DATETIME -> timestamp(3)"    "timestamp(3) without time zone" "$(ty dt)"
check "DATETIME2(6)"                "timestamp(6) without time zone" "$(ty dt2)"
check "DATETIMEOFFSET(3)"           "timestamp(3) with time zone"    "$(ty dto)"
check "UNIQUEIDENTIFIER -> uuid"    uuid                         "$(ty guid)"
check "VARBINARY -> bytea"          bytea                        "$(ty bin)"
check "PK present"                  1 "$(q "SELECT count(*) FROM pg_index WHERE indrelid = 'public.pgc_types'::regclass AND indisprimary")"
check "UNIQUE present"              1 "$(q "SELECT count(*) FROM pg_index WHERE indrelid = 'public.pgc_types'::regclass AND indisunique AND NOT indisprimary")"
check "NOT NULL kept"               t "$(q "SELECT attnotnull FROM pg_attribute WHERE attrelid = 'public.pgc_types'::regclass AND attname = 'name'")"

echo "---- values ----"
v() { q "SELECT $1 FROM public.pgc_types WHERE id = $2"; }
check "decimal(10,2)"          12345.67                         "$(v amt 1)"
check "money"                  1234.5678                        "$(v money_c 1)"
check "tinyint 255"            255                              "$(v tiny_n 1)"
check "bigint"                 123456789012345678               "$(v big_n 1)"
check "bit"                    t                                "$(v flag 1)"
check "real"                   1.5                              "$(v rl 1)"
check "float"                  2.5                              "$(v dbl 1)"
check "unicode + tab"          "Əli Vəliyev	tab"                "$(v name 1)"
check "CHAR value"             ABC                              "$(v code 1)"
check "NVARCHAR(MAX) length (100000)" 100000                    "$(v 'length(notes)' 1)"
check "DATE"                   2024-02-29                       "$(v d 1)"
check "TIME(3)"                13:14:15.123                     "$(v t 1)"
check "DATETIME"               "2024-01-02 03:04:05.123"        "$(v dt 1)"
check "DATETIME2 micros"       "2024-01-02 03:04:05.123456"     "$(v dt2 1)"
check "DATETIMEOFFSET (UTC)"   "2024-01-02 00:04:05.5+00"       "$(q "SET timezone = 'UTC'; SELECT dto FROM public.pgc_types WHERE id = 1" | tail -1)"
check "uuid"                   a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11 "$(v guid 1)"
check "VARBINARY hex"          deadbeef                         "$(v "encode(bin, 'hex')" 1)"
check "all-NULL row"           t "$(q "SELECT small_n IS NULL AND amt IS NULL AND notes IS NULL AND d IS NULL AND dto IS NULL AND bin IS NULL AND guid IS NULL FROM public.pgc_types WHERE id = 2")"
check "backslash + newline"    t "$(q "SELECT name = E'back\\\\slash\\nnewline' FROM public.pgc_types WHERE id = 3")"

echo "---- larger table (multiple COPY flushes) ----"
q "SELECT pgclone.table_from_sqlserver('$SQLSERVER_CONNINFO', '$SS_SCHEMA', 'pgc_big')" >/dev/null 2>&1
check "20000 rows"  20000 "$(q "SELECT count(*) FROM public.pgc_big")"
check "sum(id)"     200010000 "$(q "SELECT sum(id) FROM public.pgc_big")"
check "last label"  "row number 20000" "$(q "SELECT label FROM public.pgc_big WHERE id = 20000")"

echo "---- options and failure modes ----"
q "SELECT pgclone.table_from_sqlserver('$SQLSERVER_CONNINFO', '$SS_SCHEMA', 'pgc_big', include_data => false, target_table => 'pgc_nodata')" >/dev/null 2>&1
check "include_data=false -> empty table" 0 "$(q "SELECT count(*) FROM public.pgc_nodata")"

if q "SELECT pgclone.table_from_sqlserver('$SQLSERVER_CONNINFO', '$SS_SCHEMA', 'pgc_big')" >/dev/null 2>&1; then R=succeeded; else R=failed; fi
check "existing target is never overwritten" failed "$R"
check "existing target data untouched" 20000 "$(q "SELECT count(*) FROM public.pgc_big")"

ERR=$(q "SELECT pgclone.table_from_sqlserver('$SQLSERVER_CONNINFO', '$SS_SCHEMA', 'no_such_table')" 2>&1 || true)
echo "$ERR" | grep -q "not found" && R=reported || R="no message: $ERR"
check "missing SQL Server table reported" reported "$R"
check "no leftover table after failure" 0 "$(q "SELECT count(*) FROM pg_tables WHERE tablename = 'no_such_table'")"

BAD="${SQLSERVER_CONNINFO/password=*/password=wrong}"
if q "SELECT pgclone.table_from_sqlserver('$BAD', '$SS_SCHEMA', 'pgc_big', target_table => 'pgc_empty')" >/dev/null 2>&1; then R=succeeded; else R=failed; fi
check "bad password fails" failed "$R"
check "no table after failed login" 0 "$(q "SELECT count(*) FROM pg_tables WHERE tablename = 'pgc_empty'")"

echo ""
echo "SQLSERVER: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
