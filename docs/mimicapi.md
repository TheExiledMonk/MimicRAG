# MimicAPI — Thin Semantic Layer (Dev Doc)

## 0. Purpose and Non-Goals

### Purpose

MimicAPI is the only layer where semantics live.

It:
- defines what a database “means”
- coordinates requests
- composes queries
- handles joins, retries, fan-out, replication
- adapts to different backend databases

It sits above MimicDB (or any other backend).

### Non-Goals

MimicAPI does not:
- store data
- scan data
- interpret physical layout
- enforce durability
- guarantee performance

Those are backend responsibilities.

## 1. Core Architectural Rule

MimicAPI decides meaning.
Backends execute physics.

If something involves:
- policy
- choice
- coordination
- interpretation
- retries
- consistency

It belongs in MimicAPI.

## 2. High-Level Architecture

[ Application / Dialect ]
           |
           v
      MimicAPI
           |
           v
[ Backend Adapter Layer ]
           |
           +--> MimicDB
           +--> MongoDB
           +--> PostgreSQL
           +--> Other systems

The application never talks directly to the backend.

## 3. MimicAPI Core Objects

### 3.1 ApiClient

The central object.

Responsibilities:
- hold backend adapters
- manage sessions
- enforce policies
- expose a uniform API to dialects

```python
class ApiClient:
    backends: list[BackendAdapter]
    policy: ApiPolicy
```

### 3.2 BackendAdapter (Interface)

All backends must implement this interface.

```python
class BackendAdapter:
    def scan(...)
    def aggregate(...)
    def append_batch(...)
```

Adapters:
- translate MimicAPI primitives -> backend calls
- never interpret semantics
- never coordinate with other backends

### 3.3 ApiPolicy

Defines how operations behave.

```python
class ApiPolicy:
    write_policy: WritePolicy
    read_policy: ReadPolicy
    failure_policy: FailurePolicy
```

Examples:
- quorum vs best-effort
- retry limits
- fastest vs verified reads

## 4. Primitive API Contract (Core)

Everything builds on these minimal primitives.

### 4.1 Scan

```
scan(
    database: str,
    dataset: str,
    columns: list[str],
    predicates: list[Predicate],
)
```

### 4.2 Aggregate

```
aggregate(
    dataset,
    ops: { "sum": col, "count": True, ... }
)
```

### 4.3 Append (Batch)

```
append_batch(
    dataset,
    rows,
    batch_id
)
```

`batch_id` is mandatory for idempotency.

## 5. Fan-out and Replication (API-Only)

### Write path

- Send append_batch to all backends in parallel
- Wait for quorum (policy)
- Late acks are best-effort
- Fail fast if quorum impossible

### Read path

- Choose backend (fastest / healthy)
- Execute scan / aggregate
- Optional verification via secondary backend
- No backend knows it’s part of a replica set

## 6. Failure Handling

MimicAPI owns failure semantics.

- backend failures are local facts
- no backend coordination
- blacklist unhealthy backends per session
- retry via idempotent batch_id

## 7. Recovery (Client-Side)

MimicAPI maintains an append replay log.

- only for append_batch
- bounded window (v0)
- used to catch up lagging backends

Backends are unaware of recovery.

## 8. Dialect Layer (Pluggable)

A dialect is a thin adapter on top of MimicAPI.

Examples:
- Mongo-like
- SQL-ish
- Analytics-only
- Custom domain-specific DB

Dialect responsibilities:
- parse user queries
- decide joins
- decide query decomposition
- call MimicAPI primitives
- assemble results

Dialects never:
- touch backends directly
- assume backend semantics
- depend on physical layout

## 9. Joins and Composition

Joins live entirely in MimicAPI or dialect layer.

Typical pattern:
- run filtered scans
- reduce result size
- join in memory
- optionally re-query for details

Cost is explicit and paid in API layer.

## 10. Multi-Database Support

MimicAPI:
- routes requests to the correct database namespace
- enforces single-database-per-query
- handles cross-DB joins explicitly (if desired)

Backends:
- only see one database at a time

## 11. Observability

MimicAPI exposes:
- which backends were contacted
- latency per backend
- rows scanned
- segments scanned/pruned (if available)
- retries / failures

Backends remain silent executors.

## 12. Backend Swappability (Key Feature)

Because MimicAPI owns semantics:

- swapping MimicDB <-> MongoDB <-> SQL is an adapter change
- application code stays unchanged
- performance characteristics change, semantics do not

This is intentional.

## 13. Performance Contract

MimicDB guarantees maximum speed for simple primitives.
MimicAPI guarantees correct semantics.

Complex usage -> explicit performance cost.
Simple usage -> peak hardware speed.

No hidden optimizers.
No silent trade-offs.

## 14. Invariants (Must Not Be Violated)

- MimicAPI never assumes backend guarantees beyond primitives
- Backends never coordinate with each other
- Semantics never leak into backend code
- Idempotency is enforced at API layer
- Joins never enter backend execution paths

## 15. Summary

MimicAPI is:
- thin
- explicit
- powerful
- honest about trade-offs

MimicDB is:
- fast
- dumb
- durable
- architecture-friendly

Together, they form a system where:
- speed is preserved
- semantics are flexible
- architectures are swappable
- complexity never poisons the hot loop
