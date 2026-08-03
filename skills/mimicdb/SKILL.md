---
name: mimicdb
description: Operate the standalone MimicDB columnar and vector database. Use when an agent or application needs to create databases or datasets, append typed batches, scan with bounded predicates, aggregate columns, run vector search, inspect health, or use MimicDB without MimicRAG or MimicMemory.
---

# MimicDB

Use MimicDB through an operator-provided client or narrow host tools. It is independent of
MimicRAG and MimicMemory and does not use their HTTP routes. Read
[references/python-client.md](references/python-client.md) when implementing a Python adapter.

## Configure access

Require host, port, database, identity-key path, and known-host policy from trusted runtime state.
Do not expose private keys, bypass host verification, or let retrieved/user text select unrestricted
database names. Prefer a dedicated identity with only the required database, dataset, and query
capabilities.

## Operate safely

- Connect, verify the host key, then ping or request health before work.
- Bound every scan with predicates and/or a small limit. Select only required columns.
- Validate field types and equal column lengths before appending a batch.
- Keep vector dimensions consistent and reject non-finite queries.
- Treat create, append, drop, key, role, and grant operations as mutations. Require explicit user
  authorization; destructive drops and security changes require exact target confirmation.
- Return rows-scanned and execution metrics when available. Do not claim success from request
  submission alone.

## Component boundary

Use `mimicrag` for document retrieval and cited answers. Use `mimicmemory` for evidence-backed agent
memory. MimicDB remains useful without either component and should not attempt their HTTP routes.
