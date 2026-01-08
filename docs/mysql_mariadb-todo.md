# MySQL/MariaDB API Compatibility TODO

## Core Connection + Session
- [x] Connection parameters parity (host/port/user/password/db/ssl)
- [x] Connection pooling stub (API-only)
- [x] Session variables support (SQL_MODE, time_zone)
- [x] Transaction stubs (`BEGIN/COMMIT/ROLLBACK`)

## DDL Compatibility (API-level mapping)
- [x] `CREATE DATABASE`/`DROP DATABASE` stubs
- [x] `CREATE TABLE`/`DROP TABLE` (map to dataset create/delete)
- [x] `ALTER TABLE` limited support (add column only)
- [x] `SHOW DATABASES`/`SHOW TABLES`
- [x] `DESCRIBE`/`SHOW COLUMNS`

## DML Compatibility
- [x] `INSERT`/`INSERT IGNORE` mapping
- [x] `REPLACE INTO` mapping
- [x] `UPDATE`/`DELETE` mapping (API-only semantics)
- [x] `UPSERT` support (`INSERT ... ON DUPLICATE KEY UPDATE`)

## SELECT Query Surface
- [x] `SELECT` with projection + filters
- [x] `WHERE` operators parity (`=`, `!=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, `IN`, `LIKE`)
- [x] `ORDER BY` / `LIMIT` / `OFFSET`
- [x] `GROUP BY` with `COUNT/SUM/MIN/MAX`
- [x] `HAVING` basic support

## Joins (API-only)
- [x] `INNER JOIN` (API-level merge)
- [x] `LEFT JOIN` (API-level merge)
- [x] Multi-join chaining (explicitly non-optimized)

## Type Mapping
- [x] Numeric types (`INT`, `BIGINT`, `FLOAT`, `DOUBLE`)
- [x] `BOOLEAN`/`TINYINT(1)` mapping
- [x] `VARCHAR`/`TEXT` mapping (string support)
- [x] `BLOB` mapping (bytes support)
- [x] `DATETIME`/`TIMESTAMP` mapping (API-level)

## Result Shape + Metadata
- [x] Cursor-like result object
- [x] `rowcount`, `lastrowid` parity
- [x] Column metadata (name/type)
- [x] Server warnings/errors mapping

## Prepared Statements
- [x] Placeholder parsing (`?`)
- [x] Parameter binding + type conversion
- [x] Reuse plan cache (API-only)

## Compatibility Tests
- [x] DDL/DML round-trip tests
- [x] SELECT filter/operator tests
- [x] JOIN behavior tests
- [x] Type mapping tests
- [x] Error parity tests (duplicate key, bad column)
