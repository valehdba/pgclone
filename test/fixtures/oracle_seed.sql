-- Oracle seed for test/test_oracle.sh (run as the APP user via SQL*Plus).
WHENEVER SQLERROR EXIT FAILURE
SET DEFINE OFF

CREATE TABLE pgc_types (
    id       NUMBER(9)               PRIMARY KEY,
    small_n  NUMBER(4),
    big_n    NUMBER(18),
    amt      NUMBER(10,2),
    any_n    NUMBER,
    dbl      BINARY_DOUBLE,
    name     VARCHAR2(50 CHAR)       NOT NULL,
    code     CHAR(3),
    notes    CLOB,
    d        DATE,
    ts       TIMESTAMP(6),
    tstz     TIMESTAMP(6) WITH TIME ZONE,
    raw_c    RAW(16),
    email    VARCHAR2(100),
    blob_c   BLOB,
    CONSTRAINT pgc_types_email_uq UNIQUE (email)
);

INSERT INTO pgc_types VALUES (
    1, 1234, 123456789012345678, 12345.67, 3.14159, 2.5,
    'Əli Vəliyev' || CHR(9) || 'tab', 'ABC', RPAD('y', 4000, 'y'),
    TO_DATE('2024-02-29 13:14:15', 'YYYY-MM-DD HH24:MI:SS'),
    TIMESTAMP '2024-01-02 03:04:05.123456',
    TIMESTAMP '2024-01-02 03:04:05.5 +02:00',
    HEXTORAW('DEADBEEF'), 'a@x.az', UTL_RAW.CAST_TO_RAW('blob'));

-- NULLs everywhere the schema allows.
INSERT INTO pgc_types (id, name) VALUES (2, 'nulls');

-- Characters that need COPY escaping.
INSERT INTO pgc_types (id, name) VALUES (3, 'back\slash' || CHR(10) || 'newline');

-- Grow the CLOB of row 1 to 100000 characters (exercises >4000-byte values).
BEGIN
    FOR i IN 1 .. 24 LOOP
        UPDATE pgc_types SET notes = notes || RPAD('y', 4000, 'y') WHERE id = 1;
    END LOOP;
END;
/

CREATE TABLE pgc_big (id NUMBER(9) PRIMARY KEY, label VARCHAR2(40) NOT NULL);
INSERT INTO pgc_big SELECT level, 'row number ' || level FROM dual CONNECT BY level <= 20000;

COMMIT;
EXIT
