#!/usr/bin/env bash
set -euo pipefail
server_bin="${1:?server binary required}"
template="${2:?config template required}"
test_dir=$(mktemp -d); config=$(mktemp); log_file=$(mktemp)
sed "s|__DATA_PATH__|$test_dir|g" "$template" >"$config"
"$server_bin" serve --config "$config" >"$log_file" 2>&1 & server_pid=$!
cleanup() { kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; rm -rf "$test_dir" "$config" "$log_file"; }
trap cleanup EXIT
for _ in $(seq 1 100); do curl -fsS http://127.0.0.1:18086/health -H 'Authorization: Bearer operator-secret' >/dev/null 2>&1 && break; sleep 0.05; done

curl -fsS -X POST http://127.0.0.1:18086/v1/documents -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
  --data '{"tenant_id":"quality","source_uri":"quality://policy","title":"Access Policy","metadata":{"category":"policy","year":2026,"regions":["EU","US"],"authority":1.0,"source_quality":0.9},"text":"Single sign-on uses a central identity provider. SSO sessions expire after eight hours. Role based access control assigns permissions to reviewed roles."}' >/dev/null
curl -fsS -X POST http://127.0.0.1:18086/v1/documents -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
  --data '{"tenant_id":"quality","source_uri":"quality://old-note","title":"Old note","metadata":{"category":"note","year":2020,"regions":["US"],"authority":0.1,"source_quality":0.2},"text":"Legacy sessions used an unrelated manual password process."}' >/dev/null
curl -fsS -X POST http://127.0.0.1:18086/v1/documents -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
  --data '{"tenant_id":"quality","source_uri":"quality://policy-copy","title":"Policy copy","metadata":{"category":"policy","year":2026},"text":"Single sign-on uses a central identity provider. SSO sessions expire after eight hours. Role based access control assigns permissions to reviewed roles."}' >/dev/null

result=$(curl -fsS -X POST http://127.0.0.1:18086/v1/retrieve -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
  --data '{"tenant_id":"quality","query":"How long do SSO sessions last?","top_k":3,"filter":{"and":[{"field":"category","op":"eq","value":"policy"},{"field":"year","op":"gte","value":2025},{"field":"regions","op":"contains","value":"EU"}]}}')
[[ "$result" == *'service level agreement'* || "$result" == *'single sign-on'* ]]
[[ "$result" == *'"classification":"vector"'* && "$result" == *'quality://policy'* && "$result" != *'quality://old-note'* ]]
[[ "$result" == *'"confidence":'* && "$result" == *'"rerank_score":'* ]]
chunk_id=$(printf '%s' "$result" | sed -n 's/.*"chunk_id":"\([^"]*\)".*/\1/p')
trace_id=$(printf '%s' "$result" | sed -n 's/.*"trace_id":"\([^"]*\)".*/\1/p')
feedback=$(curl -fsS -X POST http://127.0.0.1:18086/v1/feedback -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
  --data "{\"tenant_id\":\"quality\",\"chunk_id\":\"$chunk_id\",\"trace_id\":\"$trace_id\",\"query\":\"SSO sessions\",\"relevant\":true}")
[[ "$feedback" == *'"accepted":true'* && "$feedback" == *'"offline_tuning"'* ]]

duplicate=$(curl -fsS -X POST http://127.0.0.1:18086/v1/retrieve -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
  --data '{"tenant_id":"quality","query":"Policy copy central identity provider","top_k":5}')
[[ "$duplicate" == *'"deduplication":{"document_id"'* && "$duplicate" == *'"duplicate":true'* ]]
missing=$(curl -fsS -X POST http://127.0.0.1:18086/v1/retrieve -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
  --data '{"tenant_id":"quality","query":"zzzxylophone nonexistent evidence","top_k":3}')
[[ "$missing" == *'"insufficient_evidence":true'* ]]

evaluation=$(curl -fsS -X POST http://127.0.0.1:18086/v1/evaluations -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
  --data '{"tenant_id":"quality","top_k":3,"cases":[{"tenant_id":"quality","query":"SSO sessions expire","relevant_source_uris":["quality://policy"]},{"tenant_id":"quality","query":"zzzxylophone nonexistent evidence","relevant_source_uris":[],"expected_insufficient":true}]}')
[[ "$evaluation" == *'"ndcg_at_k"'* && "$evaluation" == *'"mrr"'* && "$evaluation" == *'"citation_correctness"'* && "$evaluation" == *'"insufficient_evidence_accuracy"'* && "$evaluation" == *'"query_throughput_per_second"'* ]]
comparison=$(curl -fsS -X POST http://127.0.0.1:18086/v1/evaluations -H 'Authorization: Bearer operator-secret' -H 'Content-Type: application/json' \
  --data '{"tenant_id":"quality","top_k":3,"compare_modes":["fast","structured","semantic"],"cases":[{"tenant_id":"quality","query":"SSO sessions expire","relevant_source_uris":["quality://policy"]}]}')
[[ "$comparison" == *'"retention_recommendations"'* && "$comparison" == *'"fast"'* && "$comparison" == *'"structured"'* && "$comparison" == *'"semantic"'* && "$comparison" == *'"peak_memory_bytes"'* ]]
