#!/usr/bin/env bash
set -euo pipefail
server_bin="${1:?server binary required}"
template="${2:?config template required}"
test_dir=$(mktemp -d)
config=$(mktemp)
log_file=$(mktemp)
sed "s|__DATA_PATH__|$test_dir|g" "$template" >"$config"
"$server_bin" serve --config "$config" >"$log_file" 2>&1 &
server_pid=$!
cleanup() { kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; }
trap cleanup EXIT
for _ in $(seq 1 100); do
    curl -fsS http://127.0.0.1:18082/health -H 'Authorization: Bearer reader-secret' >/dev/null 2>&1 && break
    sleep 0.05
done

if curl -fsS -X POST http://127.0.0.1:18082/v1/documents -H 'Authorization: Bearer reader-secret' \
    -H 'Content-Type: application/json' --data '{"text":"denied","source_uri":"test://denied","tenant_id":"tenant-a"}' >/dev/null 2>&1; then exit 1; fi
document=$(curl -fsS -X POST http://127.0.0.1:18082/v1/documents -H 'Authorization: Bearer operator-secret' \
    -H 'Content-Type: application/json' --data '{"text":"multi scope evidence","source_uri":"test://acl","tenant_id":"tenant-a","access_scope":"confidential","access_scopes":["confidential","review"]}')
document_id=$(printf '%s' "$document" | sed -n 's/.*"document_id":"\([^"]*\)".*/\1/p')
result=$(curl -fsS -X POST http://127.0.0.1:18082/v1/retrieve -H 'Authorization: Bearer reader-secret' \
    -H 'Content-Type: application/json' --data '{"query":"multi scope evidence","tenant_id":"tenant-a","access_scopes":["review"]}')
[[ "$result" == *'test://acl'* ]]
replacement_trace=$(printf '%s' "$result" | sed -n 's/.*"trace_id":"\([^"]*\)".*/\1/p')
curl -fsS -X POST http://127.0.0.1:18082/v1/documents -H 'Authorization: Bearer operator-secret' \
    -H 'Content-Type: application/json' --data '{"text":"replacement scope evidence","source_uri":"test://acl","tenant_id":"tenant-a","access_scope":"confidential","access_scopes":["confidential","review"]}' >/dev/null
if curl -fsS "http://127.0.0.1:18082/v1/traces/$replacement_trace" -H 'Authorization: Bearer operator-secret' >/dev/null 2>&1; then exit 1; fi
if curl -fsS -X POST http://127.0.0.1:18082/v1/retrieve -H 'Authorization: Bearer reader-secret' \
    -H 'Content-Type: application/json' --data '{"query":"evidence","tenant_id":"tenant-b"}' >/dev/null 2>&1; then exit 1; fi
metrics=$(curl -fsS http://127.0.0.1:18082/metrics -H 'Authorization: Bearer operator-secret')
[[ "$metrics" == *'mimicrag_requests_total'* && "$metrics" == *'mimicrag_provider_failures_total'* ]]
curl -fsS -X POST http://127.0.0.1:18082/v1/documents -H 'Authorization: Bearer limited-secret' \
    -H 'Content-Type: application/json' --data '{"text":"first","source_uri":"test://quota-1","tenant_id":"tenant-b"}' >/dev/null
if curl -fsS -X POST http://127.0.0.1:18082/v1/documents -H 'Authorization: Bearer limited-secret' \
    -H 'Content-Type: application/json' --data '{"text":"second","source_uri":"test://quota-2","tenant_id":"tenant-b"}' >/dev/null 2>&1; then exit 1; fi
erased=$(curl -fsS -X DELETE http://127.0.0.1:18082/v1/tenants/tenant-a -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' --data '{}')
[[ "$erased" == *'"verified":true'* && "$erased" == *'"documents_deleted":1'* ]]
compacted=$(curl -fsS -X POST http://127.0.0.1:18082/v1/maintenance/compact -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' --data '{}')
[[ "$compacted" == *'"generation_switched":true'* ]]
if curl -fsS "http://127.0.0.1:18082/v1/traces" -H 'Authorization: Bearer reader-secret' >/dev/null 2>&1; then exit 1; fi
kill -TERM "$server_pid"
wait "$server_pid"
trap - EXIT
"$server_bin" inspect --config "$config" >/dev/null
snapshot_dir=$(mktemp -d)
restore_dir=$(mktemp -d)
"$server_bin" snapshot "$snapshot_dir" --config "$config" >/dev/null
"$server_bin" verify-snapshot "$snapshot_dir" --config "$config" >/dev/null
"$server_bin" restore "$snapshot_dir" --destination "$restore_dir" --rehearse --config "$config" >/dev/null
"$server_bin" restore "$snapshot_dir" --destination "$restore_dir" --config "$config" >/dev/null
"$server_bin" doctor --config "$config" >/dev/null
