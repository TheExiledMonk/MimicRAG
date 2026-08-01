#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace mimicrag {

struct ModelConfig {
    std::string provider;
    std::string model;
    std::string base_url;
    std::string api_key;
    std::string api_key_env;
    std::string api_version;
    long timeout_seconds = 60;
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
};

struct Config {
    ModelConfig chat;
    ModelConfig embedding;
    LocalEmbeddingConfig local_embedding;
    ServerConfig server;
    static Config Load(const std::string& path);
};

std::string ResolveApiKey(const ModelConfig& config);
std::string ResolveServerKey(const ServerConfig& config);

}  // namespace mimicrag
