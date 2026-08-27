#!/usr/bin/env bash
set -euo pipefail
server_bin="${1:?server binary required}"
template="${2:?config required}"
test_dir=$(mktemp -d)
config=$(mktemp)
log_file=$(mktemp)
sed "s|/tmp/mimicrag_cpp_test_data|$test_dir|g" "$template" > "$config"
"$server_bin" serve --config "$config" >"$log_file" 2>&1 &
server_pid=$!
cleanup() {
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    rm -rf "$test_dir" "$config" "$log_file"
}
trap cleanup EXIT

for _ in {1..200}; do
    if curl -fsS http://127.0.0.1:18081/health >/dev/null 2>&1; then break; fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
        cat "$log_file" >&2
        exit 1
    fi
    sleep 0.1
done
if ! curl -fsS http://127.0.0.1:18081/health >/dev/null 2>&1; then
    echo "MimicRAG server did not become healthy" >&2
    cat "$log_file" >&2
    exit 1
fi

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

document=$(curl -fsS -X POST http://127.0.0.1:18081/v1/documents -H 'Content-Type: application/json' \
    --data '{"text":"Lifecycle deletion sentinel.","source_uri":"smoke://lifecycle","tenant_id":"lifecycle"}')
document_id=$(printf '%s' "$document" | sed -n 's/.*"document_id":"\([^"]*\)".*/\1/p')
[[ -n "$document_id" ]]
before_delete=$(curl -fsS -X POST http://127.0.0.1:18081/v1/retrieve -H 'Content-Type: application/json' \
    --data '{"query":"Lifecycle deletion sentinel","tenant_id":"lifecycle","top_k":3}')
[[ "$before_delete" == *'smoke://lifecycle'* ]]
deletion_trace_id=$(printf '%s' "$before_delete" | sed -n 's/.*"trace_id":"\([^"]*\)".*/\1/p')
wrong_tenant=$(curl -fsS -X DELETE "http://127.0.0.1:18081/v1/documents/$document_id" -H 'Content-Type: application/json' \
    --data '{"tenant_id":"wrong"}')
[[ "$wrong_tenant" == *'"deleted":false'* ]]
deleted=$(curl -fsS -X DELETE "http://127.0.0.1:18081/v1/documents/$document_id" -H 'Content-Type: application/json' \
    --data '{"tenant_id":"lifecycle"}')
[[ "$deleted" == *'"deleted":true'* ]]
after_delete=$(curl -fsS -X POST http://127.0.0.1:18081/v1/retrieve -H 'Content-Type: application/json' \
    --data '{"query":"Lifecycle deletion sentinel","tenant_id":"lifecycle","top_k":3}')
[[ "$after_delete" != *'smoke://lifecycle'* ]]
if curl -fsS "http://127.0.0.1:18081/v1/traces/$deletion_trace_id" >/dev/null 2>&1; then exit 1; fi
storage=$(curl -fsS http://127.0.0.1:18081/v1/storage)
[[ "$storage" == *'"reclaimable_content_bytes":'* ]]

queued=$(curl -fsS -X POST http://127.0.0.1:18081/v1/documents -H 'Content-Type: application/json' \
    --data '{"text":"Background native embedding job.","source_uri":"smoke://job","tenant_id":"smoke","background":true}')
job_id=$(printf '%s' "$queued" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
for _ in {1..100}; do
    job=$(curl -fsS "http://127.0.0.1:18081/v1/jobs/$job_id")
    [[ "$job" == *'"status":"complete"'* ]] && break
    [[ "$job" == *'"status":"failed"'* ]] && exit 1
    sleep 0.02
done
[[ "$job" == *'"status":"complete"'* ]]

evaluation=$(curl -fsS -X POST http://127.0.0.1:18081/v1/evaluations -H 'Content-Type: application/json' \
    --data '{"top_k":3,"cases":[{"query":"native vector retrieval","relevant_source_uris":["smoke://guide"],"tenant_id":"smoke"}]}')
[[ "$evaluation" == *'"recall_at_k":1.0'* ]]

graph_text="# Graph section\\nquasargraph root $(printf 'structural context %.0s' {1..120})\\n# Details\\nquasargraph deep dive $(printf 'related evidence %.0s' {1..120})"
curl -fsS -X POST http://127.0.0.1:18081/v1/documents -H 'Content-Type: application/json' \
    --data "{\"text\":\"$graph_text\",\"source_uri\":\"smoke://graph\",\"tenant_id\":\"smoke\",\"background\":false}" >/dev/null
graph_seed=$(curl -fsS -X POST http://127.0.0.1:18081/v1/retrieve -H 'Content-Type: application/json' \
    --data '{"query":"quasargraph root","tenant_id":"smoke","top_k":3}')
graph_hits=$(printf '%s' "$graph_seed" | sed -n 's/.*"graph":{"elapsed_ms":[^,]*,"examined":[0-9]*,"hits":\([0-9]*\)}.*/\1/p')
[[ -n "$graph_hits" && "$graph_hits" -gt 0 ]]
node_id=$(printf '%s' "$graph_seed" | sed -n 's/.*"node_id":"\([^"]*\)".*/\1/p')
[[ -n "$node_id" ]]
deep_dive=$(curl -fsS -X POST http://127.0.0.1:18081/v1/graph/expand -H 'Content-Type: application/json' \
    --data "{\"node_id\":\"$node_id\",\"tenant_id\":\"smoke\",\"max_neighbors\":8}")
[[ "$deep_dive" == *'"can_expand_further":true'* ]]
[[ "$deep_dive" == *'smoke://graph'* ]]
[[ "$deep_dive" == *'"node_type":"section"'* ]]
if [[ "$deep_dive" =~ \"node_id\":\"([^\"]+)\",\"node_type\":\"section\" ]]; then
    section_id="${BASH_REMATCH[1]}"
else
    exit 1
fi
section_dive=$(curl -fsS -X POST http://127.0.0.1:18081/v1/graph/expand -H 'Content-Type: application/json' \
    --data "{\"node_id\":\"$section_id\",\"tenant_id\":\"smoke\",\"max_neighbors\":8}")
[[ "$section_dive" == *'"node_type":"chunk"'* ]]
if curl -fsS -X POST http://127.0.0.1:18081/v1/graph/expand -H 'Content-Type: application/json' \
    --data "{\"node_id\":\"$section_id\",\"tenant_id\":\"wrong-tenant\"}" >/dev/null 2>&1; then
    exit 1
fi
