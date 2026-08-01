Codex Dev Document
MimicDB UI (Desktop/Web) – v0 Implementation
Project name

mimicdb-ui

1. Objective

Build a desktop-first UI for MimicDB that allows developers to:

Connect to a MimicDB server

Browse databases and datasets

Inspect schemas

Preview data safely with predicates

Run simple aggregates

Create/drop databases and datasets

Append small batches for smoke testing

The UI must never surprise the user with expensive operations and must map directly to MimicDB primitives.

2. Hard Constraints (Do Not Violate)

No joins

No query planner

No full SQL engine

No offset-based pagination

No background auto-scans

No GPU assumptions

No server-side changes required

Compression is already handled by the server – do not duplicate it

3. Architecture (Fixed)
Desktop App (pywebview)
└── Web UI (React/Vue/Svelte)
    └── Python Service API (FastAPI)
        └── MimicDB client (existing)
            └── MimicDB server

Mandatory decisions

Desktop first

Web UI embedded via pywebview

Python service layer is required

UI never talks to MimicDB directly

4. Query Styles (User-Selectable, Same Backend)

All query styles must funnel into the same service API.

Supported styles

Mimic Native (default)

Predicate builder

Explicit aggregate controls

Mongo-style

$eq, $gt, $and semantics in UI

Translated client-side into predicate lists

SQL-style (restricted)

Very small subset:

SELECT <columns|agg>
FROM <dataset>
WHERE <simple predicates>
LIMIT <n>


Parsed client-side

No joins, GROUP BY, subqueries

SQL is syntax sugar only, not a planner.

5. Python Service API (Implement Exactly)

Base URL:

http://127.0.0.1:<port>/api

5.1 Health & connection
GET /health
→ { "ok": true, "version": "x.y.z" }

POST /connect
{
  "host": "127.0.0.1",
  "port": 9000,
  "database": "default"
}

5.2 Metadata
GET /databases
→ { "databases": [...] }

GET /datasets?database=<name>
→ { "datasets": [...] }

GET /schema?database=<name>&dataset=<name>


Schema response must include encoding info:

{
  "fields": [
    {
      "name": "value",
      "type": "int64",
      "nullable": true,
      "encoding": "dict_int32 | for_delta | raw"
    }
  ]
}

5.3 Scan (Cursor-Based Only)

Offset pagination is forbidden.

POST /scan


Request:

{
  "database": "default",
  "dataset": "events",
  "columns": ["ts", "value"],
  "predicates": [
    { "field": "value", "op": "gt", "value": 10 }
  ],
  "limit": 100,
  "cursor": null
}


Response:

{
  "columns": ["ts", "value"],
  "rows": [[169404, 12.3]],
  "cursor": "opaque-token-or-null",
  "has_more": true
}

5.4 Aggregate

Single dataset, single field only.

POST /aggregate


Response:

{
  "count": 1234,
  "sum": 4567.0,
  "min": 10.1,
  "max": 99.9,
  "has_value": true
}

5.5 Mutations (Enabled in v0)
POST /create_database
POST /drop_database
POST /create_dataset
POST /drop_dataset
POST /append


Append is manual, explicit, small-batch only.

6. UI Layout (Mandatory)
Sidebar

Server connection

Database selector

Dataset list

Main panel tabs

Schema

Data Preview

Aggregates

Ingest (optional)

Settings

7. Data Preview Rules

Default limit: 100 rows

Max limit: configurable hard cap (e.g. 10k)

Column multi-select

Predicate builder:

eq, ne, lt, le, gt, ge

is_null, not_null

Cursor-based paging only

No implicit full scans

Null rendering

Render explicitly as NULL

Subtle visual style (italic / gray)

8. Aggregate Panel Rules

One field at a time

Shares predicates with preview

Disabled if field type unsupported

Manual trigger only

9. Type Rendering Decisions
dict_int32

Default: decoded values

Toggle: “Show dictionary IDs”

Strings

Truncate to ~128 chars

Tooltip for full value

Bytes

Hex preview

Length indicator

No binary editing

10. CSV Export

Export current preview page only

Warn if truncated

Options:

Include headers

Nulls as empty or NULL

11. Desktop Packaging

Use pywebview

Launch Python service internally

Bind to localhost by default

Single binary build target

12. Security (Design-In, v0 Minimal)
v0

No auth

Localhost only by default

Forward-compatible design

Configurable bind address

Public/private key authentication planned

Request signing ready

TLS optional later

Do not block future crypto design.

13. Performance Guardrails (Required)

No auto-refresh

No implicit scans

Explicit user actions only

Schema cached in UI

Hard safety limits everywhere

14. Milestones / TODO Breakdown
M0 – Service skeleton

FastAPI app

Health + connect endpoints

M1 – Metadata

Databases

Datasets

Schema

M2 – Data preview

Scan endpoint

Cursor paging

Predicate builder

M3 – Aggregates

Aggregate endpoint

UI panel

M4 – Mutations

Create/drop db

Create/drop dataset

Append UI

M5 – Desktop build

pywebview integration

Single-binary build

15. Definition of Done (v0)

Can browse datasets

Can preview data safely

Can run aggregates

Can mutate schemas intentionally

No accidental full scans

Desktop binary runs offline

No server changes required