# MimicAPI C++ Core Migration Plan

## Goals
- Move MimicAPI execution hot paths to C++ for speed.
- Keep the server separate and dumb; MimicAPI stays a client-side layer.
- Keep Python as a thin wrapper around the C++ core.

## Phase 0: Baseline C++ Core (Done)
- C++ `ApiClientCore` with local datasets, append_batch, scan, aggregate.
- Python wrapper `CppApiClient` that forwards to the C++ core.
- Preserve the engine/segment physics by reusing `mimicdb::Dataset`.

## Phase 1: Mongo API to C++ (Next)
- Define a C++ `MongoAdapter` that targets `ApiClientCore` primitives.
- Move filter translation (`$eq/$gt/$in/$regex`) into C++.
- Move aggregation pipeline stages (`$match/$group/$project/$lookup`) into C++.
- Keep Python `MongoClient` as a thin wrapper around C++ adapter.

## Phase 2: MySQL/MariaDB API to C++ (Next)
- Define a C++ SQL parser (minimal) that emits `ApiClientCore` calls.
- Move `SELECT/INSERT/UPDATE/DELETE` parsing + execution to C++.
- Keep Python `MySQLConnection` as a thin wrapper.

## Phase 3: Adapter Interface Unification
- Define a C++ `BackendAdapter` interface for local/network transports.
- Allow ApiClientCore to target local engine or network client.
- Keep Python wrappers unchanged (just pass transport params).

## Phase 4: Optional Optimizations
- Vectorized predicate evaluation in C++ core.
- Reuse mask buffers and avoid Python allocations.
- Cache compiled predicates/SQL plans in C++.

## Non-goals
- No server-side logic moves into MimicAPI.
- No SQL planners or query optimizers in core.
- No cross-database joins at the server level.
