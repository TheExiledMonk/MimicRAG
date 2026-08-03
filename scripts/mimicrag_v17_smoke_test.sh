#!/usr/bin/env bash
set -euo pipefail
binary=$1
template=$2
root=$(mktemp -d)
config=$root/config.json
sed "s|__DATA_PATH__|$root/data|g" "$template" > "$config"
pid=""
cleanup() { [[ -z "$pid" ]] || kill "$pid" 2>/dev/null || true; rm -rf "$root"; }
trap cleanup EXIT
start() { "$binary" serve --config "$config" >"$root/server.log" 2>&1 & pid=$!; for _ in {1..100}; do curl -fsS -H 'Authorization: Bearer secret-a' http://127.0.0.1:18087/ready >/dev/null 2>&1 && return; sleep .05; done; cat "$root/server.log"; exit 1; }
stop() { kill "$pid"; wait "$pid" || true; pid=""; }
post() { curl -fsS -H 'Authorization: Bearer secret-a' -H 'Content-Type: application/json' --data "$2" "http://127.0.0.1:18087$1"; }
start
post /v1/documents '{"tenant_id":"memory","source_uri":"policy://release","text":"Authoritative release policy requires reviewed tests."}' >/dev/null
created=$(post /v1/memory/remember '{"tenant_id":"memory","namespace":"preference","subject":"release reports","statement":"Prefer concise release reports.","visibility":"private","sensitivity":"internal","allowed_purposes":["planning"],"evidence":[{"id":"evt-1","quote":"Prefer concise release reports."}],"confidence":0.9}')
memory_id=$(printf '%s' "$created" | sed -n 's/.*"memory_id":"\([^"]*\)".*/\1/p')
[[ -n "$memory_id" && "$created" == *'"status":"active"'* ]]
ordinary=$(post /v1/retrieve '{"tenant_id":"memory","query":"concise release reports","top_k":10}')
[[ "$ordinary" != *'memory://'* ]]
recalled=$(post /v1/memory/recall '{"tenant_id":"memory","query":"release report style","purpose":"planning","top_k":5}')
[[ "$recalled" == *"$memory_id"* && "$recalled" == *'"trust_domain":"memory"'* ]]
combined=$(post /v1/retrieve/combined '{"tenant_id":"memory","query":"release policy and reporting","purpose":"planning","top_k":5,"memory_top_k":5}')
[[ "$combined" == *'"trust_order":["authoritative_documents","memory_context"]'* && "$combined" == *'policy://release'* ]]
other=$(curl -fsS -H 'Authorization: Bearer secret-b' -H 'Content-Type: application/json' --data '{"tenant_id":"memory","query":"release report style","purpose":"planning"}' http://127.0.0.1:18087/v1/memory/recall)
[[ "$other" != *"$memory_id"* ]]
pending=$(post /v1/memory/remember '{"tenant_id":"memory","namespace":"semantic","subject":"legal","statement":"Legal review is pending.","sensitivity":"legal","allowed_purposes":["planning"],"evidence_ids":["evt-legal"]}')
pending_id=$(printf '%s' "$pending" | sed -n 's/.*"memory_id":"\([^"]*\)".*/\1/p')
[[ "$pending" == *'"status":"pending_confirmation"'* ]]
post /v1/memory/confirm "{\"tenant_id\":\"memory\",\"memory_id\":\"$pending_id\"}" | grep -q '"status":"active"'
corrected=$(post /v1/memory/correct "{\"tenant_id\":\"memory\",\"memory_id\":\"$memory_id\",\"statement\":\"Prefer detailed release reports.\",\"evidence_ids\":[\"evt-correction\"]}")
new_id=$(printf '%s' "$corrected" | sed -n 's/.*"memory_id":"\([^"]*\)".*/\1/p')
[[ -n "$new_id" && "$corrected" == *"\"supersedes\":\"$memory_id\""* ]]
review=$(post /v1/memory/recall '{"tenant_id":"memory","query":"detailed release reports","purpose":"planning"}')
[[ "$review" == *"$new_id"* && "$review" != *"$memory_id"* ]]
curl -fsS -X DELETE -H 'Authorization: Bearer secret-a' -H 'Content-Type: application/json' --data '{"tenant_id":"memory"}' "http://127.0.0.1:18087/v1/memory/$new_id" | grep -q '"deleted":true'
stop
start
post /v1/memory/recall '{"tenant_id":"memory","query":"legal review","purpose":"planning"}' | grep -q "$pending_id"
echo 'mimicrag V1.7 memory integration smoke passed'
