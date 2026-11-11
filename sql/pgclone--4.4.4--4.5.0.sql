/* pgclone--4.4.4--4.5.0.sql */
\echo Use "ALTER EXTENSION pgclone UPDATE" to load this file. \quit

-- v4.5.0: clone a table from Oracle into PostgreSQL.

-- Oracle -> PostgreSQL table clone (requires a build with WITH_ORACLE=1).
CREATE FUNCTION pgclone.table_from_oracle(
    oracle_conninfo TEXT,
    oracle_schema   TEXT,
    oracle_table    TEXT,
    include_data    BOOLEAN DEFAULT true,
    target_schema   TEXT    DEFAULT 'public',
    target_table    TEXT    DEFAULT NULL,
    dry_run         BOOLEAN DEFAULT false
) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_table_from_oracle'
LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pgclone.table_from_oracle(TEXT, TEXT, TEXT, BOOLEAN, TEXT, TEXT, BOOLEAN) IS 'Clone a table (structure, NOT NULL, PRIMARY KEY/UNIQUE, optionally data) from an Oracle database into PostgreSQL. oracle_conninfo: ''host=H port=1521 service=SVC user=U password=P'' or ''dsn=TNS_ALIAS user=U password=P''. dry_run returns the generated DDL without creating anything.';

-- Pure type-mapping helper (works without an Oracle client). For TIMESTAMP
-- types pass the fractional-seconds precision as data_scale.
CREATE FUNCTION pgclone.oracle_type_to_pg(
    data_type      TEXT,
    data_length    INTEGER DEFAULT NULL,
    data_precision INTEGER DEFAULT NULL,
    data_scale     INTEGER DEFAULT NULL,
    char_length    INTEGER DEFAULT NULL
) RETURNS TEXT AS 'MODULE_PATHNAME', 'pgclone_oracle_type_to_pg'
LANGUAGE C IMMUTABLE;
COMMENT ON FUNCTION pgclone.oracle_type_to_pg(TEXT, INTEGER, INTEGER, INTEGER, INTEGER) IS 'Map an Oracle column type (as in ALL_TAB_COLUMNS) to the PostgreSQL type table_from_oracle() would create; NULL if unsupported.';
