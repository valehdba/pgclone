/* pgclone--4.6.0--4.7.0.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.7.0: clone a table from MySQL / MariaDB into PostgreSQL.

-- MySQL -> PostgreSQL table clone (requires a build with WITH_MYSQL=1).
CREATE FUNCTION pgclone.table_from_mysql(
    mysql_conninfo TEXT,
    mysql_database TEXT,
    mysql_table    TEXT,
    include_data   BOOLEAN DEFAULT true,
    target_schema  TEXT    DEFAULT 'public',
    target_table   TEXT    DEFAULT NULL,
    dry_run        BOOLEAN DEFAULT false
) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table_from_mysql'
LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgclone.table_from_mysql(TEXT, TEXT, TEXT, BOOLEAN, TEXT, TEXT, BOOLEAN) IS 'Clone a table or view (structure, NOT NULL, PRIMARY KEY/UNIQUE, optionally data) from a MySQL or MariaDB database into PostgreSQL. mysql_conninfo: ''host=H port=3306 user=U password=P''; mysql_database is the MySQL database (schema) holding the table. dry_run returns the generated DDL without creating anything.';

-- Pure type-mapping helper (works without a MySQL client). The arguments are
-- the information_schema.columns columns of the same names; column_type is only
-- used to detect "unsigned". For BIT pass the width as numeric_precision; for
-- TIME/DATETIME/TIMESTAMP pass the fractional-seconds precision as
-- datetime_precision.
CREATE FUNCTION pgclone.mysql_type_to_pg(
    data_type          TEXT,
    column_type        TEXT    DEFAULT NULL,
    char_length        INTEGER DEFAULT NULL,
    numeric_precision  INTEGER DEFAULT NULL,
    numeric_scale      INTEGER DEFAULT NULL,
    datetime_precision INTEGER DEFAULT NULL
) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_mysql_type_to_pg'
LANGUAGE C IMMUTABLE;
COMMENT ON FUNCTION pgclone.mysql_type_to_pg(TEXT, TEXT, INTEGER, INTEGER, INTEGER, INTEGER) IS 'Map a MySQL/MariaDB column type (as in information_schema.columns) to the PostgreSQL type table_from_mysql() would create; NULL if unsupported.';
