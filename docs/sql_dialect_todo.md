# Unified SQL Dialect Funnel Spec (v1) TODO

Goal: normalize SQL-family dialects into one canonical execution model for MimicAPI.

Target dialects
- ANSI / Generic SQL (baseline)
- PostgreSQL
- MySQL / MariaDB (one family)
- SQLite
- Oracle SQL
- DuckDB
- Optional later: SQL Server (T-SQL subset)

## Canonical query shape
- [x] Define canonical AST for SELECT/FROM/WHERE/GROUP BY/HAVING/ORDER BY/LIMIT/OFFSET (targets: `api/mimicapi/sql_ast.py`)
- [x] Add serializer/validator for canonical AST (targets: `api/mimicapi/sql_ast.py`)
- [x] Map canonical AST to MimicAPI query primitives (targets: `api/mimicapi/sql_exec.py`)

## Dialect parsing + normalization
- [x] Add baseline SQL parser (ANSI) into canonical AST (targets: `api/mimicapi/sql_parser.py`)
- [x] Add Postgres normalizer (targets: `api/mimicapi/sql_dialects/postgres.py`)
- [x] Add MySQL/MariaDB normalizer (targets: `api/mimicapi/sql_dialects/mysql.py`)
- [x] Add SQLite normalizer (targets: `api/mimicapi/sql_dialects/sqlite.py`)
- [x] Add Oracle normalizer (targets: `api/mimicapi/sql_dialects/oracle.py`)
- [x] Add DuckDB normalizer (targets: `api/mimicapi/sql_dialects/duckdb.py`)
- [x] Add SQL Server (T-SQL subset) normalizer (targets: `api/mimicapi/sql_dialects/sqlserver.py`)
- [x] Strip quoting across dialects (`"col"`, '`col`', `[col]`) (targets: `api/mimicapi/sql_parser.py`)
- [x] Resolve aliases and positional ORDER BY at parse time (targets: `api/mimicapi/sql_parser.py`)

## SELECT / projection
- [x] Map projections and aliases to canonical form (targets: `api/mimicapi/sql_parser.py`)
- [x] Support expression projection (`expr AS alias`) (targets: `api/mimicapi/sql_ast.py`)
- [x] Support Oracle alias style (`col alias`) (targets: `api/mimicapi/sql_dialects/oracle.py`)

## WHERE / filters
- [x] Support operators: `=`, `!=`, `<>`, `<`, `<=`, `>`, `>=` (targets: `api/mimicapi/sql_parser.py`)
- [x] Support boolean logic: `AND`, `OR`, `NOT` (targets: `api/mimicapi/sql_parser.py`)
- [x] Support `IS NULL` / `IS NOT NULL` (targets: `api/mimicapi/sql_parser.py`)
- [x] Normalize `BETWEEN` into `>=`/`<=` (targets: `api/mimicapi/sql_dialects/common.py`)
- [x] Normalize `IN (...)` into OR-chain (targets: `api/mimicapi/sql_dialects/common.py`)
- [x] Normalize `NVL/IFNULL` into `COALESCE` (targets: `api/mimicapi/sql_dialects/common.py`)

## GROUP BY / HAVING
- [x] Support GROUP BY column list (targets: `api/mimicapi/sql_parser.py`)
- [x] Support Oracle alias-in-GROUP BY (targets: `api/mimicapi/sql_dialects/oracle.py`)
- [x] Define loose grouping handling (first-value or error) (targets: `api/mimicapi/sql_ast.py`)
- [x] Compile HAVING using filter evaluator (targets: `api/mimicapi/sql_exec.py`)

## Aggregates
- [x] Support COUNT(*), COUNT(col), SUM, MIN, MAX, AVG (targets: `api/mimicapi/sql_parser.py`)
- [x] Normalize COUNT(1)/COUNT(0) to COUNT (targets: `api/mimicapi/sql_dialects/common.py`)
- [x] Implement AVG as SUM/COUNT in canonical plan (targets: `api/mimicapi/sql_exec.py`)

## ORDER BY
- [x] Support `ORDER BY col ASC/DESC` (targets: `api/mimicapi/sql_parser.py`)
- [x] Resolve positional ORDER BY (Oracle `ORDER BY 1`) (targets: `api/mimicapi/sql_parser.py`)
- [x] Resolve alias ORDER BY (targets: `api/mimicapi/sql_parser.py`)

## LIMIT / OFFSET / ROW RESTRICTION
- [x] Support `LIMIT n OFFSET m` (targets: `api/mimicapi/sql_parser.py`)
- [x] Support Oracle `ROWNUM <= n` (targets: `api/mimicapi/sql_dialects/oracle.py`)
- [x] Support Oracle `FETCH FIRST n ROWS ONLY` (targets: `api/mimicapi/sql_dialects/oracle.py`)
- [x] Support SQL Server `TOP n` (targets: `api/mimicapi/sql_dialects/sqlserver.py`)

## Functions (minimal common set)
- [x] Add function registry for canonical ops (targets: `api/mimicapi/sql_functions.py`)
- [x] Map string functions: LOWER/UPPER/LENGTH/SUBSTRING/TRIM (targets: `api/mimicapi/sql_functions.py`)
- [x] Map numeric functions: ABS/ROUND/FLOOR/CEIL (targets: `api/mimicapi/sql_functions.py`)
- [x] Map null handling: COALESCE (targets: `api/mimicapi/sql_functions.py`)
- [x] Map date/time: CURRENT_DATE, CURRENT_TIMESTAMP (optional v1) (targets: `api/mimicapi/sql_functions.py`)

## Types
- [x] Define canonical type mapping table (targets: `api/mimicapi/sql_types.py`)
- [x] Map dialect types to INT64/FLOAT64/STRING/BOOL (targets: `api/mimicapi/sql_types.py`)
- [x] Enforce strict types in engine, loose in API (targets: `api/mimicapi/sql_exec.py`)

## Explicitly unsupported (must error cleanly)
- [x] Reject JOINs (targets: `api/mimicapi/sql_parser.py`)
- [x] Reject subqueries (targets: `api/mimicapi/sql_parser.py`)
- [x] Reject window functions (targets: `api/mimicapi/sql_parser.py`)
- [x] Reject stored procedures/triggers (targets: `api/mimicapi/sql_parser.py`)
- [x] Reject index/optimizer hints (targets: `api/mimicapi/sql_parser.py`)
- [x] Reject transactions/locking clauses (targets: `api/mimicapi/sql_parser.py`)

## Tests
- [x] Parser normalization tests per dialect (targets: `tests/test_sql_parser.py`)
- [x] Canonical AST equivalence tests (targets: `tests/test_sql_canonical.py`)
- [x] Execution mapping tests (targets: `tests/test_sql_exec.py`)
- [x] Unsupported feature error tests (targets: `tests/test_sql_unsupported.py`)
