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
    out.max_retries = value.value("max_retries", 2);
    if (value.contains("headers")) out.headers = value.at("headers").get<std::unordered_map<std::string, std::string>>();
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
    out.server.context_chars = server.value("context_chars", server.value("context_token_budget", size_t{4000}) * 4);
    out.server.top_k = server.value("top_k", size_t{10});
    out.server.worker_threads = server.value("worker_threads", size_t{0});
    out.server.requests_per_minute = server.value("requests_per_minute", size_t{120});
    out.server.answer_max_tokens = server.value("answer_max_tokens", size_t{1024});
    out.server.job_workers = server.value("job_workers", size_t{1});
    out.server.trace_memory = server.value("trace_memory", size_t{10000});
    out.server.trace_path = server.value("trace_path", "");
    out.server.trace_max_bytes = server.value("trace_max_bytes", uint64_t{64ULL * 1024 * 1024});
    out.server.audit_log_path = server.value("audit_log_path", out.server.data_path + "/audit.jsonl");
    out.server.audit_log_max_bytes = server.value("audit_log_max_bytes", uint64_t{64ULL * 1024 * 1024});
    out.server.capacity_warning_bytes = server.value("capacity_warning_bytes", uint64_t{1024ULL * 1024 * 1024});
    out.server.memory_warning_bytes = server.value("memory_warning_bytes", uint64_t{0});
    out.server.index_warning_bytes = server.value("index_warning_bytes", uint64_t{0});
    out.server.retention_days = server.value("retention_days", size_t{0});
    for (const auto& value : server.value("keys", json::array())) {
        ApiKeyIdentity key;
        key.id = value.at("id"); key.key = value.value("key", ""); key.key_env = value.value("key_env", "");
        key.permissions = value.value("permissions", std::vector<std::string>{"read"});
        key.tenants = value.value("tenants", std::vector<std::string>{}); key.scopes = value.value("scopes", std::vector<std::string>{});
        key.query_requests_per_minute = value.value("query_requests_per_minute", size_t{0});
        key.ingestion_requests_per_minute = value.value("ingestion_requests_per_minute", size_t{0});
        key.provider_requests_per_minute = value.value("provider_requests_per_minute", size_t{0});
        key.storage_bytes = value.value("storage_bytes", uint64_t{0});
        if (key.id.empty() || (key.key.empty() && key.key_env.empty())) throw std::runtime_error("server key identity requires id and key/key_env");
        out.server.keys.push_back(std::move(key));
    }
    const auto graph = root.value("graph", json::object());
    out.server.graph_enabled = graph.value("enabled", true);
    out.server.graph_max_seeds = graph.value("max_seeds", size_t{5});
    out.server.graph_max_neighbors = graph.value("max_neighbors", size_t{32});
    out.server.graph_max_section_children = graph.value("max_section_children", size_t{8});
    out.server.graph_min_seed_score = graph.value("min_seed_score", 0.01);
    const auto ingestion = root.value("ingestion", json::object());
    out.ingestion.default_mode = ingestion.value("default_mode", "fast");
    out.ingestion.target_chars = ingestion.value("target_chars", size_t{1600});
    out.ingestion.minimum_chars = ingestion.value("minimum_chars", size_t{240});
    out.ingestion.maximum_chars = ingestion.value("maximum_chars", size_t{2400});
    out.ingestion.overlap_chars = ingestion.value("overlap_chars", size_t{200});
    out.ingestion.maximum_chunks = ingestion.value("maximum_chunks", size_t{10000});
    out.ingestion.maximum_analysis_calls = ingestion.value("maximum_analysis_calls", size_t{8});
    out.ingestion.maximum_analysis_input_chars = ingestion.value("maximum_analysis_input_chars", size_t{24000});
    out.ingestion.maximum_generated_metadata_bytes = ingestion.value("maximum_generated_metadata_bytes", size_t{65536});
    out.ingestion.maximum_graph_edges = ingestion.value("maximum_graph_edges", size_t{200000});
    out.ingestion.maximum_analysis_seconds = ingestion.value("maximum_analysis_seconds", 60L);
    out.ingestion.analysis_enabled = ingestion.value("analysis_enabled", false);
    out.ingestion.analysis_use_chat_provider = ingestion.value("analysis_use_chat_provider", true);
    out.ingestion.prompt_version = ingestion.value("prompt_version", "semantic-ingestion-1");
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
