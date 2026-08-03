# MimicRAG V1.1 operations

V1.1 adds lifecycle, recovery, observability, and tenant controls to the native server.
Run maintenance commands with the service stopped unless the command is explicitly exposed as an
online endpoint.

## Lifecycle

- `DELETE /v1/documents/{id}` writes a durable tombstone and immediately removes the document from
  vector, lexical, and graph visibility. The JSON body must contain its `tenant_id`.
- `DELETE /v1/tenants/{id}` tombstones every live document owned by the tenant and verifies that no
  active reference remains.
- `POST /v1/maintenance/retention` applies `max_age_days`, optionally to one `tenant_id`.
- `POST /v1/maintenance/compact` drains through an exclusive maintenance gate, rejects active
  background ingestion, rewrites the catalog to live generations, rebuilds content and indexes,
  then atomically publishes the rebuilt in-memory generation.
- `GET /v1/storage` reports live, stored, and reclaimable content bytes plus disk capacity warnings.

The equivalent offline commands are `delete`, `erase-tenant`, `retention`, `stats`, `inspect`, and
`compact`. Physical erasure is complete after compaction. Backups containing erased data must be
expired according to the deployment's backup-retention policy.

## Backup and recovery

```bash
mimicrag_server snapshot /backup/mimicrag/2026-08-03 --config /etc/mimicdb/mimicrag.json
mimicrag_server verify-snapshot /backup/mimicrag/2026-08-03 --config /etc/mimicdb/mimicrag.json
mimicrag_server restore /backup/mimicrag/2026-08-03 --destination /var/lib/mimicdb/rehearsal --rehearse --config /etc/mimicdb/mimicrag.json
mimicrag_server restore /backup/mimicrag/2026-08-03 --destination /var/lib/mimicdb/restored --config /etc/mimicdb/mimicrag.json
```

Snapshots use a versioned manifest with a checksum and byte count for every file. Restore refuses a
non-empty destination. `inspect` validates catalog framing, checksums, and record structure;
`repair` removes derived content/vector/lexical files only after validating the authoritative
catalog, causing a deterministic rebuild on next start. `doctor` checks configuration, storage,
catalog, authentication, model availability, permissions, and capacity.

Persisted formats are independently versioned: catalog `MRGCAT1`/record version 1, lexical index
version 2, content manifest `MRAGTXT1`, and snapshot manifest version 1. `migrate` performs the
supported JSONL-to-MRGCAT1 migration while preserving `catalog.jsonl`; `rollback-migration` restores
that preserved legacy catalog and retains the binary catalog as `catalog.mrg.rolled_back`.

SIGINT and SIGTERM stop acceptance, drain queued requests, join workers, checkpoint indexes, and
flush trace state before exit.

## Authentication and quotas

`server.keys` replaces the legacy single bearer key with named identities:

```json
{
  "id": "agent-production",
  "key_env": "MIMICRAG_AGENT_KEY",
  "permissions": ["read", "write"],
  "tenants": ["customer-a"],
  "scopes": ["public", "internal"],
  "query_requests_per_minute": 600,
  "ingestion_requests_per_minute": 30,
  "provider_requests_per_minute": 60,
  "storage_bytes": 10737418240
}
```

Permissions are `read`, `write`, and `admin`. Empty tenant or scope lists mean all values. Documents
and requests may carry `access_scopes`; every requested scope must be granted to the key. Security
decisions and mutations are appended to `audit_log_path` as JSON with key identity, request ID,
tenant, action, and result.

Rotate a key by adding its replacement as a second identity, restarting, moving clients to the new
key, confirming the new key ID in the audit log, removing the old identity, and restarting again.
This provides an overlap window without shared-key ambiguity.

## Monitoring

`GET /metrics` exposes Prometheus counters and gauges for request volume and latency, active work,
queue depth/rejection, ingestion jobs, provider health/failures, storage, reclamation, and document
counts. JSON trace and audit logs rotate to `.1` at their configured byte limits and carry request
or trace correlation IDs.

Import [the baseline dashboard](monitoring/mimicrag-grafana-dashboard.json) and install
[the alert rules](monitoring/mimicrag-alerts.yml). Alert thresholds are starting points and should
be tuned from production baselines.
