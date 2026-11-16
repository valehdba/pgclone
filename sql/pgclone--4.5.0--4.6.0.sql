/* pgclone--4.5.0--4.6.0.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.6.0: clone a table from Microsoft SQL Server into PostgreSQL.

-- SQL Server -> PostgreSQL table clone (requires a build with WITH_SQLSERVER=1).
CREATE FUNCTION pgclone.table_from_sqlserver(
    sqlserver_conninfo TEXT,
    sqlserver_schema   TEXT,
    sqlserver_table    TEXT,
    include_data       BOOLEAN DEFAULT true,
    target_schema      TEXT    DEFAULT 'public',
    target_table       TEXT    DEFAULT NULL,
    dry_run            BOOLEAN DEFAULT false
) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table_from_sqlserver'
LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgclone.table_from_sqlserver(TEXT, TEXT, TEXT, BOOLEAN, TEXT, TEXT, BOOLEAN) IS 'Clone a table or view (structure, NOT NULL, PRIMARY KEY/UNIQUE, optionally data) from a Microsoft SQL Server database into PostgreSQL. sqlserver_conninfo: ''host=H port=1433 database=DB user=U password=P''. dry_run returns the generated DDL without creating anything.';

-- Pure type-mapping helper (works without a SQL Server client). max_length is
-- in bytes as in sys.columns (-1 = MAX). For time/datetime2/datetimeoffset
-- pass the fractional-seconds precision as "scale".
CREATE FUNCTION pgclone.sqlserver_type_to_pg(
    data_type    TEXT,
    max_length   INTEGER DEFAULT NULL,
    "precision"  INTEGER DEFAULT NULL,
    scale        INTEGER DEFAULT NULL
) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_sqlserver_type_to_pg'
LANGUAGE C IMMUTABLE;
COMMENT ON FUNCTION pgclone.sqlserver_type_to_pg(TEXT, INTEGER, INTEGER, INTEGER) IS 'Map a SQL Server column type (as in sys.columns) to the PostgreSQL type table_from_sqlserver() would create; NULL if unsupported.';
