#!/usr/bin/env bash
set -euo pipefail
binary=$1
template=$2
root=$(mktemp -d)
pid=""
cleanup() { [[ -z "$pid" ]] || kill "$pid" 2>/dev/null || true; rm -rf "$root"; }
trap cleanup EXIT

make_config() {
    local rag=$1 memory=$2 port=$3 output=$4
    sed -e "s|__DATA_PATH__|$root/data-$port|g" \
        -e "s|\"port\": 18087|\"port\": $port, \"rag_enabled\": $rag, \"memory_enabled\": $memory|" \
        "$template" > "$output"
}
start() {
    local config=$1 port=$2
    "$binary" serve --config "$config" >"$root/server-$port.log" 2>&1 & pid=$!
    for _ in {1..200}; do
        curl -fsS -H 'Authorization: Bearer secret-a' "http://127.0.0.1:$port/ready" >/dev/null 2>&1 && return
        if ! kill -0 "$pid" 2>/dev/null; then
            cat "$root/server-$port.log" >&2
            exit 1
        fi
        sleep .1
    done
    echo "MimicRAG server did not become ready on port $port" >&2
    cat "$root/server-$port.log" >&2
    exit 1
}
stop() { kill "$pid"; wait "$pid" || true; pid=""; }
status() {
    curl -sS -o "$root/response.json" -w '%{http_code}' -H 'Authorization: Bearer secret-a' \
        -H 'Content-Type: application/json' --data "$3" "http://127.0.0.1:$1$2"
}

default_config=$root/default.json
sed -e "s|__DATA_PATH__|$root/data-default|g" -e 's|"port": 18087|"port": 18090|' "$template" > "$default_config"
start "$default_config" 18090
health=$(curl -fsS -H 'Authorization: Bearer secret-a' http://127.0.0.1:18090/health)
[[ "$health" == *'"memory":true'* && "$health" == *'"rag":true'* ]]
spec=$(curl -fsS -H 'Authorization: Bearer secret-a' http://127.0.0.1:18090/openapi.json)
[[ "$spec" == *'/v1/documents'* && "$spec" == *'/v1/memory/remember'* && "$spec" == *'/v1/retrieve/combined'* ]]
stop

memory_config=$root/memory.json
make_config false true 18088 "$memory_config"
sed '/"chat":/d' "$memory_config" > "$memory_config.tmp"
mv "$memory_config.tmp" "$memory_config"
start "$memory_config" 18088
health=$(curl -fsS -H 'Authorization: Bearer secret-a' http://127.0.0.1:18088/health)
[[ "$health" == *'"memory":true'* && "$health" == *'"rag":false'* ]]
[[ $(status 18088 /v1/evidence '{"tenant_id":"memory","kind":"observation","content":"Memory-only mode works.","provenance":"test"}') == 200 ]]
[[ $(status 18088 /v1/documents '{"tenant_id":"memory","source_uri":"test://disabled","text":"disabled"}') == 503 ]]
grep -q 'MimicRAG is disabled' "$root/response.json"
spec=$(curl -fsS -H 'Authorization: Bearer secret-a' http://127.0.0.1:18088/openapi.json)
[[ "$spec" == *'/v1/memory/remember'* && "$spec" != *'/v1/documents'* && "$spec" != *'/v1/retrieve/combined'* ]]
stop

rag_config=$root/rag.json
make_config true false 18089 "$rag_config"
start "$rag_config" 18089
health=$(curl -fsS -H 'Authorization: Bearer secret-a' http://127.0.0.1:18089/health)
[[ "$health" == *'"memory":false'* && "$health" == *'"rag":true'* ]]
[[ $(status 18089 /v1/documents '{"tenant_id":"memory","source_uri":"test://rag","text":"RAG-only mode works."}') == 200 ]]
[[ $(status 18089 /v1/evidence '{"tenant_id":"memory","kind":"observation","content":"disabled","provenance":"test"}') == 503 ]]
grep -q 'MimicMemory is disabled' "$root/response.json"
[[ $(status 18089 /v1/retrieve/combined '{"tenant_id":"memory","query":"test"}') == 503 ]]
spec=$(curl -fsS -H 'Authorization: Bearer secret-a' http://127.0.0.1:18089/openapi.json)
[[ "$spec" == *'/v1/documents'* && "$spec" != *'/v1/memory/remember'* && "$spec" != *'/v1/retrieve/combined'* ]]
stop

disabled_config=$root/disabled.json
make_config false false 18091 "$disabled_config"
sed -e '/"chat":/d' -e '/"embedding":/d' "$disabled_config" > "$disabled_config.tmp"
mv "$disabled_config.tmp" "$disabled_config"
start "$disabled_config" 18091
health=$(curl -fsS -H 'Authorization: Bearer secret-a' http://127.0.0.1:18091/health)
[[ "$health" == *'"memory":false'* && "$health" == *'"rag":false'* ]]
[[ $(status 18091 /v1/documents '{"tenant_id":"memory","source_uri":"test://disabled","text":"disabled"}') == 503 ]]
[[ $(status 18091 /v1/memory/review '{"tenant_id":"memory"}') == 503 ]]
curl -fsS -H 'Authorization: Bearer secret-a' http://127.0.0.1:18091/v1/storage >/dev/null
spec=$(curl -fsS -H 'Authorization: Bearer secret-a' http://127.0.0.1:18091/openapi.json)
[[ "$spec" != *'/v1/documents'* && "$spec" != *'/v1/memory/remember'* && "$spec" == *'/v1/storage'* ]]
stop

echo 'mimicrag independent component modes passed'
