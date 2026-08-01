#!/usr/bin/env bash
set -euo pipefail

mode="auto"
output_dir="models"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --cpu|--gpu) mode="${1#--}" ;;
        --output-dir) shift; output_dir="${1:?missing output directory}" ;;
        *) echo "usage: $0 [--cpu|--gpu] [--output-dir DIR]" >&2; exit 2 ;;
    esac
    shift
done

gpu_memory_mb=0
if command -v nvidia-smi >/dev/null 2>&1; then
    gpu_memory_mb=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | sort -nr | head -1 || true)
fi
if [[ "$gpu_memory_mb" -eq 0 ]]; then
    for vram_file in /sys/class/drm/card*/device/mem_info_vram_total; do
        if [[ -r "$vram_file" ]]; then
            bytes=$(<"$vram_file")
            candidate=$((bytes / 1024 / 1024))
            (( candidate > gpu_memory_mb )) && gpu_memory_mb=$candidate
        fi
    done
fi

if [[ "$mode" == "auto" ]]; then
    if (( gpu_memory_mb >= 5120 )); then mode="gpu"; else mode="cpu"; fi
fi

if [[ "$mode" == "gpu" ]]; then
    repo="Qwen/Qwen3-Embedding-4B-GGUF"
    file="Qwen3-Embedding-4B-Q4_K_M.gguf"
    gpu_layers=-1
    minimum_bytes=2000000000
    document_prefix=""
    query_prefix='Instruct: Given a web search query, retrieve relevant passages that answer the query\nQuery: '
else
    repo="nomic-ai/nomic-embed-text-v1.5-GGUF"
    file="nomic-embed-text-v1.5.Q4_K_M.gguf"
    gpu_layers=0
    minimum_bytes=50000000
    document_prefix="search_document: "
    query_prefix="search_query: "
fi

mkdir -p "$output_dir"
target="$output_dir/$file"
url="https://huggingface.co/$repo/resolve/main/$file?download=true"
echo "Selected $mode embedding model: $repo/$file (detected GPU memory: ${gpu_memory_mb} MiB)"
curl --fail --location --retry 4 --continue-at - --output "$target" "$url"

size=$(wc -c < "$target")
if (( size < minimum_bytes )); then
    echo "downloaded model is unexpectedly small ($size bytes)" >&2
    exit 1
fi
magic=$(od -An -tx1 -N4 "$target" | tr -d ' \n')
if [[ "$magic" != "47475546" ]]; then
    echo "downloaded file is not a GGUF model" >&2
    exit 1
fi

fragment="$output_dir/mimicrag.local_embedding.json"
printf '{\n  "local_embedding": {\n    "enabled": true,\n    "eager_dual_index": true,\n    "model_path": "%s",\n    "gpu_layers": %s,\n    "threads": 0,\n    "context_size": 2048,\n    "document_prefix": "%s",\n    "query_prefix": "%s"\n  }\n}\n' "$target" "$gpu_layers" "$document_prefix" "$query_prefix" > "$fragment"
echo "Verified GGUF model: $target"
echo "Configuration fragment: $fragment"
