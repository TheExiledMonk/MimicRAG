#include "mimicrag/http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace mimicrag {
namespace {
using json = nlohmann::json;

std::atomic<bool> shutdown_requested{false};
std::atomic<int> listening_socket{-1};
std::atomic<uint64_t> requests_total{0}, requests_active{0}, request_latency_us{0}, request_latency_max_us{0};
std::atomic<uint64_t> queue_depth{0}, queue_rejected{0};
std::array<std::atomic<uint64_t>, 7> latency_buckets{};

void RequestShutdown(int) {
    shutdown_requested = true;
    const int listener = listening_socket.exchange(-1);
    if (listener >= 0) ::close(listener);
}

struct RequestTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    RequestTimer() { ++requests_total; ++requests_active; }
    ~RequestTimer() {
        --requests_active;
        const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        request_latency_us += elapsed;
        auto maximum = request_latency_max_us.load();
        while (elapsed > maximum && !request_latency_max_us.compare_exchange_weak(maximum, elapsed)) {}
        const std::array<uint64_t, 6> limits{1000, 5000, 10000, 50000, 100000, 500000};
        size_t bucket = 0; while (bucket < limits.size() && elapsed > limits[bucket]) ++bucket; ++latency_buckets[bucket];
    }
};

void SendAll(int socket, const std::string& value) {
    size_t sent = 0;
    while (sent < value.size()) {
        const ssize_t count = ::send(socket, value.data() + sent, value.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) return;
        sent += static_cast<size_t>(count);
    }
}

void Reply(int socket, int status, const json& body, const std::string& content_type = "application/json") {
    const std::string encoded = body.dump();
    std::ostringstream response;
    response << "HTTP/1.1 " << status << (status == 200 ? " OK" : status == 401 ? " Unauthorized" : status == 404 ? " Not Found" : " Bad Request") << "\r\nContent-Type: " << content_type << "\r\nContent-Length: " << encoded.size() << "\r\nConnection: close\r\n\r\n" << encoded;
    SendAll(socket, response.str());
}

void ReplyText(int socket, int status, const std::string& body, const std::string& content_type) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << " OK\r\nContent-Type: " << content_type << "\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\n\r\n" << body;
    SendAll(socket, response.str());
}

std::string Metrics(RagEngine& engine) {
    const auto storage = engine.StorageStats(); const auto operational = engine.OperationalMetrics(); const auto health = engine.Health(); const auto total = requests_total.load();
    uint64_t resident_bytes = 0; { std::ifstream statm("/proc/self/statm"); uint64_t total_pages = 0, resident_pages = 0;
        if (statm >> total_pages >> resident_pages) resident_bytes = resident_pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)); }
    std::ostringstream out;
    out << "# TYPE mimicrag_requests_total counter\nmimicrag_requests_total " << total << '\n'
        << "# TYPE mimicrag_requests_active gauge\nmimicrag_requests_active " << requests_active.load() << '\n'
        << "# TYPE mimicrag_request_latency_seconds_sum counter\nmimicrag_request_latency_seconds_sum " << request_latency_us.load() / 1e6 << '\n'
        << "# TYPE mimicrag_request_latency_seconds_max gauge\nmimicrag_request_latency_seconds_max " << request_latency_max_us.load() / 1e6 << '\n'
        << "# TYPE mimicrag_queue_depth gauge\nmimicrag_queue_depth " << queue_depth.load() << '\n'
        << "# TYPE mimicrag_queue_rejected_total counter\nmimicrag_queue_rejected_total " << queue_rejected.load() << '\n'
        << "# TYPE mimicrag_storage_bytes gauge\nmimicrag_storage_bytes " << storage.at("total_storage_bytes") << '\n'
        << "# TYPE mimicrag_reclaimable_content_bytes gauge\nmimicrag_reclaimable_content_bytes " << storage.at("reclaimable_content_bytes") << '\n'
        << "# TYPE mimicrag_live_documents gauge\nmimicrag_live_documents " << storage.at("live_documents") << '\n';
    out << "# TYPE mimicrag_request_latency_seconds histogram\n";
    const std::array<const char*, 7> labels{"0.001", "0.005", "0.01", "0.05", "0.1", "0.5", "+Inf"};
    uint64_t cumulative = 0; for (size_t i = 0; i < labels.size(); ++i) { cumulative += latency_buckets[i].load(); out << "mimicrag_request_latency_seconds_bucket{le=\"" << labels[i] << "\"} " << cumulative << '\n'; }
    out << "mimicrag_request_latency_seconds_count " << total << '\n';
    out << "# TYPE mimicrag_ingestion_jobs_queued gauge\nmimicrag_ingestion_jobs_queued " << operational.at("jobs_queued") << '\n'
        << "# TYPE mimicrag_ingestion_jobs_running gauge\nmimicrag_ingestion_jobs_running " << operational.at("jobs_running") << '\n'
        << "# TYPE mimicrag_provider_failures_total counter\nmimicrag_provider_failures_total " << operational.at("provider_failures") << '\n'
        << "# TYPE mimicrag_provider_healthy gauge\nmimicrag_provider_healthy " << (operational.at("remote_provider_healthy").get<bool>() ? 1 : 0) << '\n';
    out << "# TYPE mimicrag_embedding_latency_seconds_sum counter\nmimicrag_embedding_latency_seconds_sum " << operational.at("embedding_latency_us").get<uint64_t>() / 1e6 << '\n'
        << "# TYPE mimicrag_embedding_latency_seconds_count counter\nmimicrag_embedding_latency_seconds_count " << operational.at("embedding_calls") << '\n'
        << "# TYPE mimicrag_mapped_bytes gauge\nmimicrag_mapped_bytes " << operational.at("mapped_lexical_bytes") << '\n'
        << "# TYPE mimicrag_lexical_terms_cached gauge\nmimicrag_lexical_terms_cached " << operational.at("lexical_terms_cached") << '\n';
    out << "# TYPE mimicrag_resident_memory_bytes gauge\nmimicrag_resident_memory_bytes " << resident_bytes << '\n'
        << "# TYPE mimicrag_lexical_index_bytes gauge\nmimicrag_lexical_index_bytes " << health.at("lexical_index_bytes") << '\n'
        << "# TYPE mimicrag_content_bytes gauge\nmimicrag_content_bytes " << health.at("content_bytes") << '\n'
        << "# TYPE mimicrag_vector_rows gauge\nmimicrag_vector_rows " << health.at("remote_vector_rows").get<uint64_t>() + health.at("local_vector_rows").get<uint64_t>() << '\n';
    return out.str();
}

void StartSse(int socket) {
    SendAll(socket, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nX-Accel-Buffering: no\r\nConnection: close\r\n\r\n");
}

bool ConstantEqual(const std::string& left, const std::string& right) {
    size_t difference = left.size() ^ right.size();
    const size_t count = std::max(left.size(), right.size());
    for (size_t i = 0; i < count; ++i) difference |= static_cast<unsigned char>(i < left.size() ? left[i] : 0) ^ static_cast<unsigned char>(i < right.size() ? right[i] : 0);
    return difference == 0;
}

bool Contains(const std::vector<std::string>& values, const std::string& value) {
    return values.empty() || std::find(values.begin(), values.end(), value) != values.end();
}

std::string KeySecret(const ApiKeyIdentity& key) {
    if (!key.key.empty()) return key.key;
    const char* value = std::getenv(key.key_env.c_str()); return value ? value : "";
}

struct AuthContext { std::string id = "legacy"; const ApiKeyIdentity* key = nullptr; };

bool HasPermission(const AuthContext& auth, const std::string& permission) {
    return !auth.key || std::find(auth.key->permissions.begin(), auth.key->permissions.end(), "admin") != auth.key->permissions.end() ||
        std::find(auth.key->permissions.begin(), auth.key->permissions.end(), permission) != auth.key->permissions.end();
}

void Audit(const ServerConfig& config, const AuthContext& auth, const std::string& action,
           const std::string& tenant, bool allowed, const std::string& request_id) {
    if (config.audit_log_path.empty()) return;
    static std::mutex mutex; std::lock_guard lock(mutex);
    const auto path = std::filesystem::path(config.audit_log_path);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    if (config.audit_log_max_bytes && std::filesystem::exists(path) && std::filesystem::file_size(path) >= config.audit_log_max_bytes) {
        const auto rotated = std::filesystem::path(path.string() + ".1"); std::filesystem::remove(rotated); std::filesystem::rename(path, rotated);
    }
    std::ofstream output(path, std::ios::app);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    output << json{{"timestamp_ms", now}, {"request_id", request_id}, {"key_id", auth.id}, {"action", action},
        {"tenant_id", tenant}, {"allowed", allowed}}.dump() << '\n';
}

bool RateAllowed(const std::string& identity, size_t limit) {
    if (!limit) return true;
    static std::mutex mutex;
    static std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> identities;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    auto& events = identities[identity];
    while (!events.empty() && now - events.front() >= std::chrono::minutes(1)) events.pop_front();
    if (events.size() >= limit) return false;
    events.push_back(now);
    return true;
}

std::string Lower(std::string value) { for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return value; }

json OpenApiSpec() {
    const std::vector<std::pair<std::string, std::string>> routes{
        {"/health", "get"}, {"/ready", "get"}, {"/openapi.json", "get"},
        {"/metrics", "get"},
        {"/v1/documents", "post"}, {"/v1/documents/{document_id}", "delete"},
        {"/v1/retrieve", "post"}, {"/v1/answers", "post"}, {"/v1/graph/expand", "post"},
        {"/v1/feedback", "post"}, {"/v1/evaluations", "post"}, {"/v1/storage", "get"},
        {"/v1/traces", "get"}, {"/v1/traces/{trace_id}", "get"},
        {"/v1/jobs/{job_id}", "get"}, {"/v1/tenants/{tenant_id}", "delete"},
        {"/v1/maintenance/retention", "post"}, {"/v1/maintenance/compact", "post"},
        {"/v1/chat/completions", "post"}};
    json paths = json::object();
    for (const auto& [path, method] : routes) paths[path][method] = {{"responses", {{"200", {{"description", "Successful response"}}}}}};
    paths["/v1/jobs/{job_id}"]["delete"] = {{"responses", {{"200", {{"description", "Cancellation result"}}}}}};
    return {{"openapi", "3.1.0"}, {"info", {{"title", "MimicRAG HTTP API"}, {"version", "1.6.0"}}},
        {"paths", std::move(paths)}, {"components", {{"securitySchemes", {{"bearerAuth", {{"type", "http"}, {"scheme", "bearer"}}}}}}}};
}

void Handle(int socket, RagEngine& engine, std::shared_mutex& engine_gate) {
    RequestTimer request_timer;
    try {
        std::string request; char buffer[8192]; size_t header_end = std::string::npos;
        while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t count = recv(socket, buffer, sizeof(buffer), 0); if (count <= 0) throw std::runtime_error("short HTTP request"); request.append(buffer, static_cast<size_t>(count));
            if (request.size() > 65536) throw std::runtime_error("HTTP headers too large");
        }
        std::istringstream headers(request.substr(0, header_end)); std::string method, path, version; headers >> method >> path >> version; std::string line; std::getline(headers, line);
        std::unordered_map<std::string, std::string> fields;
        while (std::getline(headers, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); const auto colon = line.find(':'); if (colon != std::string::npos) { auto value = line.substr(colon + 1); while (!value.empty() && value.front() == ' ') value.erase(value.begin()); fields[Lower(line.substr(0, colon))] = value; } }
        const size_t length = fields.count("content-length") ? std::stoull(fields["content-length"]) : 0;
        if (length > engine.GetConfig().server.max_body_bytes) { Reply(socket, 400, {{"error", "request body too large"}}); return; }
        std::string body = request.substr(header_end + 4);
        while (body.size() < length) { const ssize_t count = recv(socket, buffer, sizeof(buffer), 0); if (count <= 0) break; body.append(buffer, static_cast<size_t>(count)); }
        std::string supplied = fields["authorization"]; if (supplied.rfind("Bearer ", 0) == 0) supplied.erase(0, 7);
        const auto& server_config = engine.GetConfig().server; AuthContext auth; bool authenticated = server_config.keys.empty();
        if (!server_config.keys.empty()) {
            for (const auto& key : server_config.keys) if (ConstantEqual(supplied, KeySecret(key))) { auth = {key.id, &key}; authenticated = true; break; }
        } else { const std::string expected = ResolveServerKey(server_config); authenticated = expected.empty() || ConstantEqual(supplied, expected); }
        if (!authenticated) { Reply(socket, 401, {{"error", "invalid API key"}}); return; }
        const std::string identity = auth.key ? auth.id : (supplied.empty() ? fields["x-forwarded-for"] : "legacy");
        if (!RateAllowed(identity, server_config.requests_per_minute)) { Reply(socket, 429, {{"error", "rate limit exceeded"}}); return; }
        const std::string permission = (method == "POST" && (path == "/v1/documents" || path == "/v1/feedback")) || (method == "DELETE" && path.rfind("/v1/documents/", 0) == 0) ? "write" :
            (path == "/metrics" || path == "/v1/storage" || path.rfind("/v1/jobs", 0) == 0 || path.rfind("/v1/traces", 0) == 0 || path.rfind("/v1/tenants", 0) == 0 || path.rfind("/v1/maintenance", 0) == 0 || path == "/v1/evaluations") ? "admin" : "read";
        if (!HasPermission(auth, permission)) { Audit(server_config, auth, permission, "", false, fields["x-request-id"]); Reply(socket, 403, {{"error", "permission denied"}}); return; }
        std::unique_lock<std::shared_mutex> maintenance_lock;
        std::shared_lock<std::shared_mutex> request_lock;
        if (method == "POST" && path == "/v1/maintenance/compact") maintenance_lock = std::unique_lock(engine_gate);
        else request_lock = std::shared_lock(engine_gate);
        if (method == "GET" && (path == "/health" || path == "/ready")) { Reply(socket, 200, engine.Health()); return; }
        if (method == "GET" && path == "/openapi.json") { Reply(socket, 200, OpenApiSpec()); return; }
        if (method == "GET" && path == "/metrics") { ReplyText(socket, 200, Metrics(engine), "text/plain; version=0.0.4"); return; }
        if (method == "GET" && path == "/v1/storage") { Reply(socket, 200, engine.StorageStats()); return; }
        if (method == "GET" && path.rfind("/v1/jobs/", 0) == 0) { Reply(socket, 200, engine.Job(path.substr(9))); return; }
        if (method == "GET" && path.rfind("/v1/traces/", 0) == 0) { Reply(socket, 200, engine.Trace(path.substr(11))); return; }
        if (method == "GET" && path.rfind("/v1/traces", 0) == 0) { size_t limit = 100; const auto marker = path.find("limit="); if (marker != std::string::npos) limit = std::stoull(path.substr(marker + 6)); Reply(socket, 200, {{"traces", engine.RecentTraces(std::min<size_t>(limit, 1000))}}); return; }
        json input = body.empty() ? json::object() : json::parse(body);
        input["_request_id"] = fields["x-request-id"].empty() ? "req-" + std::to_string(requests_total.load()) : fields["x-request-id"];
        const std::string tenant = path.rfind("/v1/tenants/", 0) == 0 ? path.substr(12) : input.value("tenant_id", "default");
        const std::string scope = input.value("access_scope", input.value("metadata", json::object()).value("access_scope", "public"));
        const bool tenant_scoped = path.rfind("/v1/documents", 0) == 0 || path.rfind("/v1/retrieve", 0) == 0 ||
            path.rfind("/v1/answers", 0) == 0 || path.rfind("/v1/chat/completions", 0) == 0 || path.rfind("/v1/graph", 0) == 0 ||
            path.rfind("/v1/evaluations", 0) == 0 || path.rfind("/v1/feedback", 0) == 0 || path.rfind("/v1/tenants", 0) == 0 || path == "/v1/maintenance/retention";
        bool scopes_allowed = Contains(auth.key ? auth.key->scopes : std::vector<std::string>{}, scope);
        if (auth.key && input.contains("access_scopes")) for (const auto& requested : input["access_scopes"])
            scopes_allowed &= requested.is_string() && Contains(auth.key->scopes, requested.get<std::string>());
        if (auth.key && tenant_scoped && (!Contains(auth.key->tenants, tenant) || !scopes_allowed)) {
            Audit(server_config, auth, permission, tenant, false, fields["x-request-id"]); Reply(socket, 403, {{"error", "tenant or scope denied"}}); return;
        }
        const bool ingestion = method == "POST" && path == "/v1/documents";
        const bool provider = path == "/v1/answers" || path == "/v1/chat/completions";
        const size_t quota = !auth.key ? 0 : ingestion ? auth.key->ingestion_requests_per_minute : provider ? auth.key->provider_requests_per_minute : auth.key->query_requests_per_minute;
        if (quota && !RateAllowed(auth.id + "\n" + tenant + "\n" + (ingestion ? "ingest" : provider ? "provider" : "query"), quota)) {
            Audit(server_config, auth, "quota", tenant, false, fields["x-request-id"]); Reply(socket, 429, {{"error", "tenant quota exceeded"}}); return;
        }
        if (ingestion && auth.key && auth.key->storage_bytes && engine.TenantStorageBytes(tenant) + body.size() > auth.key->storage_bytes) {
            Audit(server_config, auth, "storage_quota", tenant, false, fields["x-request-id"]); Reply(socket, 429, {{"error", "storage quota exceeded"}}); return;
        }
        if (ingestion || method == "DELETE" || path.rfind("/v1/maintenance", 0) == 0)
            Audit(server_config, auth, ingestion ? "document.ingest" : path.rfind("/v1/tenants", 0) == 0 ? "tenant.erase" :
                method == "DELETE" ? "document.delete" : "maintenance", tenant, true, input.value("_request_id", ""));
        if (method == "DELETE" && path.rfind("/v1/jobs/", 0) == 0) Reply(socket, 200, engine.CancelJob(path.substr(9)));
        else if (method == "POST" && path == "/v1/documents") {
            json sanitized = input;
            for (const auto& internal : {"_replay", "_operation", "_record_version", "ingested_at_ms", "analysis_results", "remote_embeddings", "local_embeddings", "remote_model_identity", "local_model_identity"}) sanitized.erase(internal);
            Reply(socket, 200, engine.Ingest(sanitized));
        }
        else if (method == "DELETE" && path.rfind("/v1/documents/", 0) == 0) {
            json request = input; request["document_id"] = path.substr(14); Reply(socket, 200, engine.DeleteDocument(request));
        }
        else if (method == "DELETE" && path.rfind("/v1/tenants/", 0) == 0) {
            json request = input; request["tenant_id"] = path.substr(12); Reply(socket, 200, engine.EraseTenant(request));
        }
        else if (method == "POST" && path == "/v1/maintenance/retention") Reply(socket, 200, engine.ApplyRetention(input));
        else if (method == "POST" && path == "/v1/maintenance/compact") Reply(socket, 200, engine.CompactOnline());
        else if (method == "POST" && path == "/v1/retrieve") Reply(socket, 200, engine.Retrieve(input));
        else if (method == "POST" && path == "/v1/feedback") Reply(socket, 200, engine.RecordFeedback(input));
        else if (method == "POST" && path == "/v1/evaluations") Reply(socket, 200, engine.Evaluate(input));
        else if (method == "POST" && path == "/v1/graph/expand") Reply(socket, 200, engine.GraphExpand(input));
        else if (method == "POST" && path == "/v1/answers") {
            if (input.value("stream", false)) { StartSse(socket); try { auto result = engine.AnswerStream(input, [&](const std::string& token) { SendAll(socket, "data: " + json({{"type", "token"}, {"token", token}}).dump() + "\n\n"); }); SendAll(socket, "data: " + json({{"type", "complete"}, {"trace_id", result["trace_id"]}, {"citations", result["citations"]}}).dump() + "\n\ndata: [DONE]\n\n"); } catch (const std::exception& error) { SendAll(socket, "data: " + json({{"type", "error"}, {"error", error.what()}}).dump() + "\n\ndata: [DONE]\n\n"); } }
            else Reply(socket, 200, engine.Answer(input));
        }
        else if (method == "POST" && path == "/v1/chat/completions") {
            std::string query; for (auto it = input.at("messages").rbegin(); it != input.at("messages").rend(); ++it) if (it->value("role", "") == "user") { query = it->value("content", ""); break; }
            json options = json::object(); for (const auto& key : {"max_tokens", "temperature", "top_p", "stop"}) if (input.contains(key)) options[key] = input[key];
            json adapted = {{"query", query}, {"tenant_id", input.value("tenant_id", "default")}, {"access_scope", input.value("access_scope", "public")}, {"top_k", input.value("top_k", engine.GetConfig().server.top_k)}, {"options", options}, {"conversation", input.at("messages")}, {"_request_id", input.value("_request_id", "")}};
            if (input.contains("access_scopes")) adapted["access_scopes"] = input["access_scopes"];
            if (input.value("stream", false)) {
                StartSse(socket); try { engine.AnswerStream(adapted, [&](const std::string& token) { SendAll(socket, "data: " + json({{"id", "chatcmpl-cpp"}, {"object", "chat.completion.chunk"}, {"model", engine.GetConfig().chat.model}, {"choices", json::array({{{"index", 0}, {"delta", {{"content", token}}}, {"finish_reason", nullptr}}})}}).dump() + "\n\n"); }); } catch (const std::exception& error) { SendAll(socket, "data: " + json({{"error", {{"message", error.what()}, {"type", "provider_error"}}}}).dump() + "\n\n"); } SendAll(socket, "data: [DONE]\n\n");
            } else { auto answer = engine.Answer(adapted); Reply(socket, 200, {{"id", "chatcmpl-cpp"}, {"object", "chat.completion"}, {"model", engine.GetConfig().chat.model}, {"choices", json::array({{{"index", 0}, {"message", {{"role", "assistant"}, {"content", answer["answer"]}}}, {"finish_reason", "stop"}}})}, {"mimicrag", {{"trace_id", answer["trace_id"]}, {"citations", answer["citations"]}, {"embedding_backend", answer["embedding_backend"]}}}}); }
        } else Reply(socket, 404, {{"error", "not found"}});
    } catch (const std::out_of_range& error) { Reply(socket, 404, {{"error", error.what()}}); }
      catch (const std::exception& error) { Reply(socket, 400, {{"error", error.what()}}); }
    ::close(socket);
}
}  // namespace

void HttpServer::Run() {
    const int listener = socket(AF_INET, SOCK_STREAM, 0); if (listener < 0) throw std::runtime_error("socket failed");
    shutdown_requested = false; listening_socket = listener;
    std::signal(SIGINT, RequestShutdown); std::signal(SIGTERM, RequestShutdown);
    int reuse = 1; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(engine_.GetConfig().server.port);
    if (inet_pton(AF_INET, engine_.GetConfig().server.host.c_str(), &address.sin_addr) != 1) throw std::runtime_error("invalid IPv4 bind address");
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 128) != 0) throw std::runtime_error("bind/listen failed: " + std::string(std::strerror(errno)));
    std::cout << "mimicrag_server listening on " << engine_.GetConfig().server.host << ':' << engine_.GetConfig().server.port << "\n";
    std::mutex queue_mutex;
    std::condition_variable queue_ready;
    std::deque<int> queue;
    std::shared_mutex engine_gate;
    const size_t workers = engine_.GetConfig().server.worker_threads ? engine_.GetConfig().server.worker_threads : std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (size_t i = 0; i < workers; ++i) pool.emplace_back([&] {
        for (;;) {
            int client;
            { std::unique_lock lock(queue_mutex); queue_ready.wait(lock, [&] { return shutdown_requested || !queue.empty(); });
              if (queue.empty() && shutdown_requested) return;
              client = queue.front(); queue.pop_front(); queue_depth = queue.size(); }
            Handle(client, engine_, engine_gate);
        }
    });
    while (!shutdown_requested) {
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) { if (shutdown_requested) break; continue; }
        std::lock_guard lock(queue_mutex);
        if (queue.size() >= 1024) { ++queue_rejected; Reply(client, 503, {{"error", "server busy"}}); ::close(client); }
        else { queue.push_back(client); queue_depth = queue.size(); queue_ready.notify_one(); }
    }
    shutdown_requested = true; queue_ready.notify_all();
    for (auto& worker : pool) if (worker.joinable()) worker.join();
    listening_socket = -1;
}
}  // namespace mimicrag
