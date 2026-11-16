#!/bin/bash
# ============================================================
# Integration test for pgclone.table_from_mysql().
# Needs: pgclone built with WITH_MYSQL=1, a MySQL (or MariaDB) server seeded with
# test/fixtures/mysql_seed.sql, and a target PostgreSQL (PGHOST/PGPORT/...).
#
#   MYSQL_CONNINFO="host=localhost port=3306 user=root password=..." \
#   PGHOST=localhost PGPORT=5434 PGUSER=postgres bash test/test_mysql.sh
# ============================================================
set -euo pipefail

: "${MYSQL_CONNINFO:?set MYSQL_CONNINFO}"
PGDATABASE="${PGDATABASE:-target_db}"
export PGDATABASE
export PGCLIENTENCODING=UTF8
MY_DB="${MY_DB:-pgc_test}"

PASS=0; FAIL=0
q() { psql -X -v ON_ERROR_STOP=1 -tA -c "$1"; }
check() {   # check "description" "expected" "actual"
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1 (expected [$2], got [$3])"; FAIL=$((FAIL + 1)); fi
}

q "DROP TABLE IF EXISTS public.pgc_types, public.pgc_big, public.pgc_empty, public.pgc_nodata" >/dev/null
q "CREATE EXTENSION IF NOT EXISTS pgclone" >/dev/null

echo "---- dry run ----"
DDL=$(q "SELECT pgclone.table_from_mysql('$MYSQL_CONNINFO', '$MY_DB', 'pgc_types', dry_run => true)" 2>/dev/null)
echo "$DDL" | grep -q "CREATE TABLE public.pgc_types" && R=ok || R=missing
check "dry_run returns CREATE TABLE" ok "$R"
echo "$DDL" | grep -q "PRIMARY KEY (id)" && R=ok || R=missing
check "dry_run includes PRIMARY KEY" ok "$R"
echo "$DDL" | grep -q "UNIQUE (email)" && R=ok || R=missing
check "dry_run includes UNIQUE" ok "$R"
echo "$DDL" | grep -q "geo" && R=present || R=absent
check "unsupported POINT column skipped" absent "$R"
check "dry_run creates nothing" 0 "$(q "SELECT count(*) FROM pg_tables WHERE tablename = 'pgc_types'")"

echo "---- clone with data ----"
q "SELECT pgclone.table_from_mysql('$MYSQL_CONNINFO', '$MY_DB', 'pgc_types')" >/dev/null 2>&1
check "row count" 4 "$(q "SELECT count(*) FROM public.pgc_types")"

echo "---- column types ----"
ty() { q "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'public.pgc_types'::regclass AND attname = '$1'"; }
check "INT -> integer"              integer                      "$(ty id)"
check "TINYINT UNSIGNED"            smallint                     "$(ty tiny_u)"
check "SMALLINT"                    smallint                     "$(ty small_n)"
check "MEDIUMINT -> integer"        integer                      "$(ty med_n)"
check "INT UNSIGNED -> bigint"      bigint                       "$(ty uint_n)"
check "BIGINT"                      bigint                       "$(ty big_n)"
check "BIGINT UNSIGNED -> numeric(20)" "numeric(20,0)"           "$(ty ubig_n)"
check "TINYINT(1) -> smallint"      smallint                     "$(ty flag)"
check "DECIMAL(10,2) -> numeric"    "numeric(10,2)"              "$(ty amt)"
check "FLOAT -> real"               real                         "$(ty rl)"
check "DOUBLE"                      "double precision"           "$(ty dbl)"
check "BIT(5)"                      "bit(5)"                     "$(ty bits)"
check "VARCHAR(50)"                 "character varying(50)"      "$(ty name)"
check "CHAR(3)"                     "character(3)"               "$(ty code)"
check "LONGTEXT -> text"            text                         "$(ty notes)"
check "ENUM -> text"                text                         "$(ty status)"
check "SET -> text"                 text                         "$(ty tags)"
check "JSON -> jsonb"               jsonb                        "$(ty js)"
check "YEAR -> smallint"            smallint                     "$(ty yr)"
check "DATE"                        date                         "$(ty d)"
check "TIME(3) -> interval(3)"      "interval(3)"                "$(ty t)"
check "DATETIME(6)"                 "timestamp(6) without time zone" "$(ty dt)"
check "TIMESTAMP(3)"                "timestamp(3) with time zone"    "$(ty ts)"
check "VARBINARY -> bytea"          bytea                        "$(ty bin)"
check "BLOB -> bytea"               bytea                        "$(ty blb)"
check "PK present"                  1 "$(q "SELECT count(*) FROM pg_index WHERE indrelid = 'public.pgc_types'::regclass AND indisprimary")"
check "UNIQUE present"              1 "$(q "SELECT count(*) FROM pg_index WHERE indrelid = 'public.pgc_types'::regclass AND indisunique AND NOT indisprimary")"
check "NOT NULL kept"               t "$(q "SELECT attnotnull FROM pg_attribute WHERE attrelid = 'public.pgc_types'::regclass AND attname = 'name'")"

echo "---- values ----"
v() { q "SELECT $1 FROM public.pgc_types WHERE id = $2"; }
check "tinyint unsigned 255"   255                              "$(v tiny_u 1)"
check "mediumint 8388607"      8388607                          "$(v med_n 1)"
check "int unsigned max"       4294967295                       "$(v uint_n 1)"
check "bigint"                 123456789012345678               "$(v big_n 1)"
check "bigint unsigned max"    18446744073709551615             "$(v ubig_n 1)"
check "tinyint(1)"             1                                "$(v flag 1)"
check "decimal(10,2)"          12345.67                         "$(v amt 1)"
check "float"                  1.5                              "$(v rl 1)"
check "double"                 2.5                              "$(v dbl 1)"
check "bit(5)"                 10101                            "$(v bits 1)"
check "unicode + tab"          "Əli Vəliyev	tab"                "$(v name 1)"
check "CHAR value"             ABC                              "$(v code 1)"
check "LONGTEXT length (100000)" 100000                         "$(v 'length(notes)' 1)"
check "ENUM"                   done                             "$(v status 1)"
check "SET"                    a,c                              "$(v tags 1)"
check "JSON"                   '{"k": [1, 2]}'                  "$(v js 1)"
check "YEAR"                   2024                             "$(v yr 1)"
check "DATE"                   2024-02-29                       "$(v d 1)"
check "TIME negative, fractional" "-12:34:56.789"               "$(v t 1)"
check "DATETIME micros"        "2024-01-02 03:04:05.123456"     "$(v dt 1)"
check "TIMESTAMP (UTC)"        "2024-01-02 03:04:05.5+00"       "$(q "SET timezone = 'UTC'; SELECT ts FROM public.pgc_types WHERE id = 1" | tail -1)"
check "VARBINARY hex"          deadbeef                         "$(v "encode(bin, 'hex')" 1)"
check "BLOB hex"               cafe                             "$(v "encode(blb, 'hex')" 1)"
check "all-NULL row"           t "$(q "SELECT small_n IS NULL AND amt IS NULL AND notes IS NULL AND d IS NULL AND ts IS NULL AND bin IS NULL AND bits IS NULL FROM public.pgc_types WHERE id = 2")"
check "backslash + newline"    t "$(q "SELECT name = E'back\\\\slash\\nnewline' FROM public.pgc_types WHERE id = 3")"
check "zero dates become NULL" t "$(q "SELECT d IS NULL AND dt IS NULL FROM public.pgc_types WHERE id = 4")"

echo "---- larger table (multiple COPY flushes) ----"
q "SELECT pgclone.table_from_mysql('$MYSQL_CONNINFO', '$MY_DB', 'pgc_big')" >/dev/null 2>&1
check "20000 rows"  20000 "$(q "SELECT count(*) FROM public.pgc_big")"
check "sum(id)"     200010000 "$(q "SELECT sum(id) FROM public.pgc_big")"
check "last label"  "row number 20000" "$(q "SELECT label FROM public.pgc_big WHERE id = 20000")"

echo "---- options and failure modes ----"
q "SELECT pgclone.table_from_mysql('$MYSQL_CONNINFO', '$MY_DB', 'pgc_big', include_data => false, target_table => 'pgc_nodata')" >/dev/null 2>&1
check "include_data=false -> empty table" 0 "$(q "SELECT count(*) FROM public.pgc_nodata")"

if q "SELECT pgclone.table_from_mysql('$MYSQL_CONNINFO', '$MY_DB', 'pgc_big')" >/dev/null 2>&1; then R=succeeded; else R=failed; fi
check "existing target is never overwritten" failed "$R"
check "existing target data untouched" 20000 "$(q "SELECT count(*) FROM public.pgc_big")"

ERR=$(q "SELECT pgclone.table_from_mysql('$MYSQL_CONNINFO', '$MY_DB', 'no_such_table')" 2>&1 || true)
echo "$ERR" | grep -q "not found" && R=reported || R="no message: $ERR"
check "missing MySQL table reported" reported "$R"
check "no leftover table after failure" 0 "$(q "SELECT count(*) FROM pg_tables WHERE tablename = 'no_such_table'")"

BAD="${MYSQL_CONNINFO/password=*/password=wrong}"
if q "SELECT pgclone.table_from_mysql('$BAD', '$MY_DB', 'pgc_big', target_table => 'pgc_empty')" >/dev/null 2>&1; then R=succeeded; else R=failed; fi
check "bad password fails" failed "$R"
check "no table after failed login" 0 "$(q "SELECT count(*) FROM pg_tables WHERE tablename = 'pgc_empty'")"

echo ""
echo "MYSQL: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
