# MimicRAG deployment guide

This guide covers a single-node Linux deployment of the native `mimicrag_server`.
MimicRAG currently provides process-level durability and recovery, not replication or
automatic failover.

## Filesystem layout

A conventional installation uses:

```text
/opt/mimicdb/                         executable and repository checkout
/etc/mimicdb/mimicrag.json           non-secret configuration
/etc/mimicdb/mimicrag.env            API keys, mode 0600
/var/lib/mimicdb/mimicrag/           catalog, content, indexes, traces
/var/lib/mimicdb/models/              local GGUF embedding model
```

Create a dedicated unprivileged account:

```bash
sudo useradd --system --home /var/lib/mimicdb --shell /usr/bin/nologin mimicrag
sudo install -d -o mimicrag -g mimicrag -m 0750 \
  /etc/mimicdb /var/lib/mimicdb/mimicrag /var/lib/mimicdb/models
sudo install -o root -g mimicrag -m 0640 mimicrag.json /etc/mimicdb/mimicrag.json
```

Set these paths in the configuration:

```json
{
  "server": {
    "data_path": "/var/lib/mimicdb/mimicrag",
    "trace_path": "/var/lib/mimicdb/mimicrag/traces.jsonl"
  },
  "local_embedding": {
    "model_path": "/var/lib/mimicdb/models/embedding.gguf"
  }
}
```

## Secrets

Do not place keys in the JSON file. Reference environment variables with
`api_key_env` and create `/etc/mimicdb/mimicrag.env`:

```text
MIMICRAG_API_KEY=replace-with-a-long-random-value
OPENAI_API_KEY=replace-with-provider-key
```

```bash
sudo chown root:mimicrag /etc/mimicdb/mimicrag.env
sudo chmod 0640 /etc/mimicdb/mimicrag.env
```

Rotate a provider key by updating the environment file and restarting the service.
Server keys are named identities with permissions, tenant/scope bindings, and quotas.
Use an overlapping old/new identity during rotation as described in
[V1.1 operations](operations_v1_1.md#authentication-and-quotas).

## systemd

Install and adapt the supplied unit:

```bash
sudo cp deploy/mimicrag_cpp.service /etc/systemd/system/mimicrag.service
sudo systemctl daemon-reload
sudo systemctl enable --now mimicrag
sudo systemctl status mimicrag
```

Readiness check:

```bash
set -a
. /etc/mimicdb/mimicrag.env
set +a
curl -fsS http://127.0.0.1:8080/ready \
  -H "Authorization: Bearer $MIMICRAG_API_KEY"
```

The health response reports chunk/document counts, vector rows, graph size, local
embedding device, catalog format, disk-backed content bytes, lexical-index mode, and
whether sealed vectors remain resident.

## TLS and network exposure

The native listener is HTTP over IPv4. Do not expose it directly to an untrusted
network. Bind it to loopback and use a TLS reverse proxy. The proxy should:

- Require TLS 1.2 or newer
- Preserve the `Authorization` header
- Set explicit request-body and header limits
- Apply connection and request timeouts
- Avoid buffering server-sent events on `/v1/answers` and `/v1/chat/completions`
- Export access/error metrics without logging bearer tokens or request bodies

Network firewalls should allow the service to contact only configured model providers
and required package/model sources.

## Backup and restore

Use the native checksummed snapshot, verification, rehearsal, and restore commands documented in
[V1.1 operations](operations_v1_1.md#backup-and-recovery). The manual stopped-service procedure
below remains available for older installations.

Treat the complete `server.data_path` as one consistency unit. The safest initial
procedure is a stopped-service snapshot:

```bash
sudo systemctl stop mimicrag
sudo rsync -aH --delete /var/lib/mimicdb/mimicrag/ /backup/mimicrag/current/
sudo systemctl start mimicrag
```

The authoritative append log is `catalog.mrg`. `content.dat`, IVF files, and
`lexical.idx` are derived or generation-checked, but copying everything avoids an
expensive rebuild. Preserve configuration and the exact local embedding model as part
of disaster recovery.

Restore into an empty data directory while the service is stopped, fix ownership,
start the service, wait for `/ready`, and run the golden-query evaluation suite. Test
this procedure before relying on it.

## Upgrades

Before upgrading:

1. Record the current commit and compiler/build options.
2. Stop ingestion and take a verified backup.
3. Build the new executable separately.
4. Run its unit and smoke tests.
5. Start it against a copied data directory first.
6. Check `/health`, retrieval, graph expansion, and a golden evaluation set.
7. Keep the previous binary and backup until the validation window closes.

Do not assume index or catalog downgrades are supported unless the target versions have
been explicitly tested together.

## Monitoring

At minimum monitor:

- Process availability and `/ready`
- RSS, private memory, GPU memory, disk usage, and inode usage
- Request rate, p50/p95/p99 latency, HTTP error rate, and rate-limit responses
- Provider errors/timeouts and local embedding fallback rate
- Ingestion job failures and backlog
- Catalog/content/index file growth
- Trace volume and filesystem capacity
- Golden-query recall, answer terms, and citation rate across releases

Alert before the filesystem containing the data path becomes full. Index publication
uses temporary files and therefore needs free space beyond the final index size.

## Capacity planning

Run a representative pilot with real documents. Synthetic vectors alone do not reveal
chunk-size distribution, lexical vocabulary, graph density, provider latency, or answer
quality. Measure after a warm restart and under the expected concurrency, tenant
filters, and ingestion rate.

The recorded 10,000-article Wikipedia test is a useful reference, not a sizing
guarantee. Keep headroom for mapped pages, temporary index builds, model buffers,
concurrent response assembly, and operating-system cache.
