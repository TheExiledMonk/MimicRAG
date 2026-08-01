#!/usr/bin/env bash
set -euo pipefail
server_bin="${1:?server binary required}"
config="${2:?config required}"
log_file=$(mktemp)
"$server_bin" "$config" >"$log_file" 2>&1 &
server_pid=$!
cleanup() { kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; }
trap cleanup EXIT

for _ in $(seq 1 100); do
    if curl -fsS http://127.0.0.1:18081/health >/dev/null 2>&1; then break; fi
    sleep 0.05
done

health=$(curl -fsS http://127.0.0.1:18081/health)
[[ "$health" == *'"implementation":"c++"'* ]]
curl -fsS -X POST http://127.0.0.1:18081/v1/documents -H 'Content-Type: application/json' \
    --data '{"text":"Native MimicDB vector retrieval and BM25.","source_uri":"smoke://guide","tenant_id":"smoke"}' >/dev/null
result=$(curl -fsS -X POST http://127.0.0.1:18081/v1/retrieve -H 'Content-Type: application/json' \
    --data '{"query":"native vector retrieval","tenant_id":"smoke","top_k":3}')
[[ "$result" == *'"embedding_backend":"bm25"'* ]]
[[ "$result" == *'smoke://guide'* ]]
trace_id=$(printf '%s' "$result" | sed -n 's/.*"trace_id":"\([^"]*\)".*/\1/p')
trace=$(curl -fsS "http://127.0.0.1:18081/v1/traces/$trace_id")
[[ "$trace" == *'"operation":"retrieve"'* ]]

queued=$(curl -fsS -X POST http://127.0.0.1:18081/v1/documents -H 'Content-Type: application/json' \
    --data '{"text":"Background native embedding job.","source_uri":"smoke://job","tenant_id":"smoke","background":true}')
job_id=$(printf '%s' "$queued" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
for _ in $(seq 1 100); do
    job=$(curl -fsS "http://127.0.0.1:18081/v1/jobs/$job_id")
    [[ "$job" == *'"status":"complete"'* ]] && break
    [[ "$job" == *'"status":"failed"'* ]] && exit 1
    sleep 0.02
done
[[ "$job" == *'"status":"complete"'* ]]

evaluation=$(curl -fsS -X POST http://127.0.0.1:18081/v1/evaluations -H 'Content-Type: application/json' \
    --data '{"top_k":3,"cases":[{"query":"native vector retrieval","relevant_source_uris":["smoke://guide"],"tenant_id":"smoke"}]}')
[[ "$evaluation" == *'"recall_at_k":1.0'* ]]
