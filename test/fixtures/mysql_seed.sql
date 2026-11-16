-- MySQL seed for test/test_mysql.sh (run with the mysql client as root).
-- Re-runnable: drops and recreates the fixture tables.
-- Non-ASCII characters are built from UTF-8 bytes (UNHEX) so the seed does not
-- depend on the client character set.
CREATE DATABASE IF NOT EXISTS pgc_test CHARACTER SET utf8mb4;
USE pgc_test;
SET SESSION time_zone = '+00:00';
SET SESSION cte_max_recursion_depth = 20000;

DROP TABLE IF EXISTS pgc_types;
DROP TABLE IF EXISTS pgc_big;

CREATE TABLE pgc_types (
    id       INT                 NOT NULL PRIMARY KEY,
    tiny_u   TINYINT UNSIGNED    NULL,
    small_n  SMALLINT            NULL,
    med_n    MEDIUMINT           NULL,
    uint_n   INT UNSIGNED        NULL,
    big_n    BIGINT              NULL,
    ubig_n   BIGINT UNSIGNED     NULL,
    flag     TINYINT(1)          NULL,
    amt      DECIMAL(10,2)       NULL,
    rl       FLOAT               NULL,
    dbl      DOUBLE              NULL,
    bits     BIT(5)              NULL,
    name     VARCHAR(50)         NOT NULL,
    code     CHAR(3)             NULL,
    notes    LONGTEXT            NULL,
    status   ENUM('new','done')  NULL,
    tags     SET('a','b','c')    NULL,
    js       JSON                NULL,
    yr       YEAR                NULL,
    d        DATE                NULL,
    t        TIME(3)             NULL,
    dt       DATETIME(6)         NULL,
    ts       TIMESTAMP(3)        NULL DEFAULT NULL,
    bin      VARBINARY(16)       NULL,
    blb      BLOB                NULL,
    email    VARCHAR(100)        NULL,
    geo      POINT               NULL,
    UNIQUE KEY uq_pgc_types_email (email)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO pgc_types
    (id, tiny_u, small_n, med_n, uint_n, big_n, ubig_n, flag, amt, rl, dbl, bits,
     name, code, notes, status, tags, js, yr, d, t, dt, ts, bin, blb, email, geo)
VALUES
    (1, 255, 123, 8388607, 4294967295, 123456789012345678, 18446744073709551615,
     1, 12345.67, 1.5, 2.5, b'10101',
     CONCAT(CONVERT(UNHEX('C68F') USING utf8mb4), 'li V',
            CONVERT(UNHEX('C999') USING utf8mb4), 'liyev', CHAR(9), 'tab'),
     'ABC', REPEAT('x', 100000), 'done', 'a,c', '{"k": [1, 2]}', 2024,
     '2024-02-29', '-12:34:56.789', '2024-01-02 03:04:05.123456',
     '2024-01-02 03:04:05.500', 0xDEADBEEF, 0xCAFE, 'a@x.az',
     ST_GeomFromText('POINT(1 1)'));
INSERT INTO pgc_types (id, name) VALUES (2, 'all nulls');
INSERT INTO pgc_types (id, name, email)
VALUES (3, CONCAT('back\\slash', CHAR(10), 'newline'), 'b@x.az');

-- Zero dates are only storable with a relaxed sql_mode.
SET SESSION sql_mode = '';
INSERT INTO pgc_types (id, name, d, dt) VALUES (4, 'zero dates', '0000-00-00', '0000-00-00 00:00:00');

CREATE TABLE pgc_big (
    id    INT          NOT NULL PRIMARY KEY,
    label VARCHAR(40)  NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO pgc_big (id, label)
WITH RECURSIVE n (i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20000)
SELECT i, CONCAT('row number ', i) FROM n;
