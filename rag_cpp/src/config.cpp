#include "mimicrag/config.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace mimicrag {
namespace {
using json = nlohmann::json;

ModelConfig ReadModel(const json& value) {
    ModelConfig out;
    out.provider = value.value("provider", "openai_compatible");
    out.model = value.value("model", "");
    out.base_url = value.value("base_url", "");
    out.api_key = value.value("api_key", "");
    out.api_key_env = value.value("api_key_env", "");
    out.api_version = value.value("api_version", "");
    out.timeout_seconds = value.value("timeout_seconds", 60L);
    if (out.model.empty()) throw std::runtime_error("model is required");
    if (out.base_url.empty()) {
        static const std::unordered_map<std::string, std::string> defaults = {
            {"openai", "https://api.openai.com/v1"}, {"anthropic", "https://api.anthropic.com/v1"},
            {"google", "https://generativelanguage.googleapis.com/v1beta"}, {"cohere", "https://api.cohere.com/v2"},
            {"ollama", "http://127.0.0.1:11434"}, {"groq", "https://api.groq.com/openai/v1"},
            {"mistral", "https://api.mistral.ai/v1"}, {"xai", "https://api.x.ai/v1"},
            {"deepseek", "https://api.deepseek.com/v1"}, {"together", "https://api.together.xyz/v1"}};
        auto found = defaults.find(out.provider);
        if (found != defaults.end()) out.base_url = found->second;
    }
    return out;
}
}  // namespace

Config Config::Load(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open config: " + path);
    nlohmann::json root;
    input >> root;
    Config out;
    out.chat = ReadModel(root.at("chat"));
    out.embedding = ReadModel(root.at("embedding"));
    const auto local = root.value("local_embedding", json::object());
    out.local_embedding.enabled = local.value("enabled", false);
    out.local_embedding.eager_dual_index = local.value("eager_dual_index", true);
    out.local_embedding.model_path = local.value("model_path", "");
    out.local_embedding.gpu_layers = local.value("gpu_layers", -1);
    out.local_embedding.threads = local.value("threads", 0);
    out.local_embedding.context_size = local.value("context_size", 2048);
    out.local_embedding.document_prefix = local.value("document_prefix", "");
    out.local_embedding.query_prefix = local.value("query_prefix", "");
    const auto server = root.value("server", json::object());
    out.server.host = server.value("host", "127.0.0.1");
    out.server.port = static_cast<uint16_t>(server.value("port", 8080));
    out.server.api_key = server.value("api_key", "");
    out.server.api_key_env = server.value("api_key_env", "");
    out.server.data_path = server.value("data_path", "data/mimicrag_cpp");
    out.server.max_body_bytes = server.value("max_body_bytes", size_t{10 * 1024 * 1024});
    out.server.max_query_chars = server.value("max_query_chars", size_t{16000});
    out.server.context_chars = server.value("context_chars", size_t{16000});
    out.server.top_k = server.value("top_k", size_t{10});
    out.server.worker_threads = server.value("worker_threads", size_t{0});
    out.server.requests_per_minute = server.value("requests_per_minute", size_t{120});
    if (out.local_embedding.enabled && out.local_embedding.model_path.empty()) {
        throw std::runtime_error("local_embedding.model_path is required when enabled");
    }
    return out;
}

std::string ResolveApiKey(const ModelConfig& config) {
    if (!config.api_key.empty()) return config.api_key;
    if (!config.api_key_env.empty()) {
        const char* value = std::getenv(config.api_key_env.c_str());
        if (value) return value;
    }
    return {};
}

std::string ResolveServerKey(const ServerConfig& config) {
    if (!config.api_key.empty()) return config.api_key;
    if (!config.api_key_env.empty()) {
        const char* value = std::getenv(config.api_key_env.c_str());
        if (value) return value;
    }
    return {};
}
}  // namespace mimicrag
