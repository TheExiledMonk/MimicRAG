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
cleanup() { kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; rm -rf "$test_dir" "$config" "$log_file"; }
trap cleanup EXIT
for _ in {1..200}; do
    curl -fsS http://127.0.0.1:18084/health -H 'Authorization: Bearer operator-secret' >/dev/null 2>&1 && break
    if ! kill -0 "$server_pid" 2>/dev/null; then
        cat "$log_file" >&2
        exit 1
    fi
    sleep 0.1
done
if ! curl -fsS http://127.0.0.1:18084/health -H 'Authorization: Bearer operator-secret' >/dev/null 2>&1; then
    echo "MimicRAG server did not become healthy" >&2
    cat "$log_file" >&2
    exit 1
fi

structured=$(curl -fsS -X POST http://127.0.0.1:18084/v1/documents \
    -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
    --data '{"tenant_id":"test","source_uri":"test://structured.md","format":"markdown","mode":"structured","title":"Policy","text":"# Returns\n\nReturns require a receipt and must be requested within thirty days.\n\n## Exceptions\n\n- Clearance items are final sale.\n- Defective products may be exchanged.\n\n| Region | Window |\n| --- | --- |\n| EU | 14 days |"}')
[[ "$structured" == *'"mode":"structured"'* && "$structured" == *'"format":"markdown"'* && "$structured" == *'"parser_version"'* ]]
retrieved=$(curl -fsS -X POST http://127.0.0.1:18084/v1/retrieve \
    -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
    --data '{"tenant_id":"test","query":"clearance final sale","top_k":3}')
[[ "$retrieved" == *'"section_path":"Returns > Exceptions"'* && "$retrieved" == *'"chunking_strategy":"structured-adaptive-v1"'* && "$retrieved" == *'"page_start":0'* ]]

semantic=$(curl -fsS -X POST http://127.0.0.1:18084/v1/documents \
    -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
    --data '{"tenant_id":"test","source_uri":"test://semantic.md","format":"markdown","mode":"semantic","text":"# Dense section\n\nAlpha defines the primary rule. Beta qualifies the rule for archived records. Gamma provides the audit exception. Delta explains how reviewers record their decision. Epsilon states that source evidence remains authoritative."}')
[[ "$semantic" == *'"mode":"semantic"'* && "$semantic" == *'"enabled":false'* && "$semantic" == *'"chunk_count"'* ]]

queued=$(curl -fsS -X POST http://127.0.0.1:18084/v1/documents \
    -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
    --data '{"tenant_id":"test","source_uri":"test://background.txt","mode":"structured","background":true,"text":"Background ingestion preserves its source text and reports durable progress."}')
job_id=$(printf '%s' "$queued" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
[[ -n "$job_id" && "$queued" == *'"status":"queued"'* ]]
for _ in {1..100}; do
    job=$(curl -fsS "http://127.0.0.1:18084/v1/jobs/$job_id" -H 'Authorization: Bearer operator-secret')
    [[ "$job" == *'"progress":'* && "$job" == *'"stage":'* ]]
    [[ "$job" == *'"status":"complete"'* ]] && break
    sleep 0.02
done
[[ "$job" == *'"status":"complete"'* && "$job" == *'"progress":1.0'* ]]

# Cancellation is idempotent for a job that has already reached a terminal state.
cancelled=$(curl -fsS -X DELETE "http://127.0.0.1:18084/v1/jobs/$job_id" \
    -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' --data '{}')
[[ "$cancelled" == *'"status":"complete"'* ]]
test ! -d "$test_dir/ingestion_checkpoints" || [[ -z "$(find "$test_dir/ingestion_checkpoints" -name '*.json' -print -quit)" ]]
