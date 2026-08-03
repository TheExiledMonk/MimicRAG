#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mimicrag {

struct ModelConfig {
    std::string provider;
    std::string model;
    std::string base_url;
    std::string api_key;
    std::string api_key_env;
    std::string api_version;
    long timeout_seconds = 60;
    int max_retries = 2;
    std::unordered_map<std::string, std::string> headers;
};

struct LocalEmbeddingConfig {
    bool enabled = false;
    bool eager_dual_index = true;
    std::string model_path;
    int gpu_layers = -1;
    int threads = 0;
    int context_size = 2048;
    std::string document_prefix;
    std::string query_prefix;
};

struct ApiKeyIdentity {
    std::string id;
    std::string key;
    std::string key_env;
    std::vector<std::string> permissions;
    std::vector<std::string> tenants;
    std::vector<std::string> scopes;
    size_t query_requests_per_minute = 0;
    size_t ingestion_requests_per_minute = 0;
    size_t provider_requests_per_minute = 0;
    uint64_t storage_bytes = 0;
};

struct IngestionConfig {
    std::string default_mode = "fast";
    size_t target_chars = 1600;
    size_t minimum_chars = 240;
    size_t maximum_chars = 2400;
    size_t overlap_chars = 200;
    size_t maximum_chunks = 10000;
    size_t maximum_analysis_calls = 8;
    size_t maximum_analysis_input_chars = 24000;
    size_t maximum_generated_metadata_bytes = 65536;
    size_t maximum_graph_edges = 200000;
    long maximum_analysis_seconds = 60;
    bool analysis_enabled = false;
    bool analysis_use_chat_provider = true;
    std::string prompt_version = "semantic-ingestion-1";
};

struct ServerConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    std::string api_key;
    std::string api_key_env;
    std::string data_path = "data/mimicrag_cpp";
    size_t max_body_bytes = 10 * 1024 * 1024;
    size_t max_query_chars = 16000;
    size_t context_chars = 16000;
    size_t top_k = 10;
    size_t worker_threads = 0;
    size_t requests_per_minute = 120;
    size_t answer_max_tokens = 1024;
    size_t job_workers = 1;
    size_t trace_memory = 10000;
    std::string trace_path;
    uint64_t trace_max_bytes = 64ULL * 1024 * 1024;
    std::string audit_log_path;
    uint64_t audit_log_max_bytes = 64ULL * 1024 * 1024;
    uint64_t capacity_warning_bytes = 1024ULL * 1024 * 1024;
    uint64_t memory_warning_bytes = 0;
    uint64_t index_warning_bytes = 0;
    size_t retention_days = 0;
    std::vector<ApiKeyIdentity> keys;
    bool graph_enabled = true;
    size_t graph_max_seeds = 5;
    size_t graph_max_neighbors = 32;
    size_t graph_max_section_children = 8;
    double graph_min_seed_score = 0.01;
};

struct Config {
    ModelConfig chat;
    ModelConfig embedding;
    LocalEmbeddingConfig local_embedding;
    ServerConfig server;
    IngestionConfig ingestion;
    static Config Load(const std::string& path);
};

std::string ResolveApiKey(const ModelConfig& config);
std::string ResolveServerKey(const ServerConfig& config);

}  // namespace mimicrag
