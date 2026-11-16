-- SQL Server seed for test/test_sqlserver.sh (run with FreeTDS tsql as sa).
-- Re-runnable: drops and recreates the fixture tables.
-- Non-ASCII characters are built with NCHAR() so the seed does not depend on
-- the encoding of the client that sends it.
IF DB_ID('pgc_test') IS NULL CREATE DATABASE pgc_test;
GO
ALTER DATABASE pgc_test SET ALLOW_SNAPSHOT_ISOLATION ON;
GO
USE pgc_test;
GO
DROP TABLE IF EXISTS dbo.pgc_types;
DROP TABLE IF EXISTS dbo.pgc_big;
GO
CREATE TABLE dbo.pgc_types (
    id       INT                NOT NULL PRIMARY KEY,
    small_n  SMALLINT           NULL,
    tiny_n   TINYINT            NULL,
    big_n    BIGINT             NULL,
    flag     BIT                NULL,
    amt      DECIMAL(10,2)      NULL,
    money_c  MONEY              NULL,
    rl       REAL               NULL,
    dbl      FLOAT              NULL,
    name     NVARCHAR(50)       NOT NULL,
    code     CHAR(3)            NULL,
    vtxt     VARCHAR(20)        NULL,
    notes    NVARCHAR(MAX)      NULL,
    d        DATE               NULL,
    t        TIME(3)            NULL,
    dt       DATETIME           NULL,
    dt2      DATETIME2(6)       NULL,
    dto      DATETIMEOFFSET(3)  NULL,
    guid     UNIQUEIDENTIFIER   NULL,
    bin      VARBINARY(16)      NULL,
    email    NVARCHAR(100)      NULL CONSTRAINT uq_pgc_types_email UNIQUE,
    geo      GEOGRAPHY          NULL
);
GO
INSERT INTO dbo.pgc_types
    (id, small_n, tiny_n, big_n, flag, amt, money_c, rl, dbl, name, code, vtxt,
     notes, d, t, dt, dt2, dto, guid, bin, email, geo)
VALUES
    (1, 123, 255, 123456789012345678, 1, 12345.67, 1234.5678, 1.5, 2.5,
     NCHAR(399) + N'li V' + NCHAR(601) + N'liyev' + NCHAR(9) + N'tab',
     'ABC', 'plain',
     REPLICATE(CAST(N'x' AS NVARCHAR(MAX)), 100000),
     '2024-02-29', '13:14:15.123', '2024-01-02 03:04:05.123',
     '2024-01-02 03:04:05.123456', '2024-01-02 03:04:05.500 +03:00',
     'A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11', 0xDEADBEEF, N'a@x.az',
     geography::Point(40.4, 49.8, 4326));
INSERT INTO dbo.pgc_types (id, name) VALUES (2, N'all nulls');
INSERT INTO dbo.pgc_types (id, name, email)
VALUES (3, N'back\slash' + NCHAR(10) + N'newline', N'b@x.az');
GO
CREATE TABLE dbo.pgc_big (
    id    INT           NOT NULL PRIMARY KEY,
    label NVARCHAR(40)  NOT NULL
);
GO
INSERT INTO dbo.pgc_big (id, label)
SELECT TOP (20000) n, N'row number ' + CAST(n AS NVARCHAR(10))
FROM (SELECT ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS n
      FROM sys.all_objects a CROSS JOIN sys.all_objects b) x;
GO
