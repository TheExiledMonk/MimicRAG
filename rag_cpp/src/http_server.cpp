#include "mimicrag/http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace mimicrag {
namespace {
using json = nlohmann::json;

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

void StartSse(int socket) {
    SendAll(socket, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nX-Accel-Buffering: no\r\nConnection: close\r\n\r\n");
}

bool ConstantEqual(const std::string& left, const std::string& right) {
    size_t difference = left.size() ^ right.size();
    const size_t count = std::max(left.size(), right.size());
    for (size_t i = 0; i < count; ++i) difference |= static_cast<unsigned char>(i < left.size() ? left[i] : 0) ^ static_cast<unsigned char>(i < right.size() ? right[i] : 0);
    return difference == 0;
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

void Handle(int socket, RagEngine& engine) {
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
        const std::string expected = ResolveServerKey(engine.GetConfig().server);
        std::string supplied = fields["authorization"]; if (supplied.rfind("Bearer ", 0) == 0) supplied.erase(0, 7);
        if (!expected.empty() && !ConstantEqual(supplied, expected)) { Reply(socket, 401, {{"error", "invalid API key"}}); return; }
        const std::string identity = supplied.empty() ? fields["x-forwarded-for"] : supplied;
        if (!RateAllowed(identity, engine.GetConfig().server.requests_per_minute)) { Reply(socket, 429, {{"error", "rate limit exceeded"}}); return; }
        if (method == "GET" && (path == "/health" || path == "/ready")) { Reply(socket, 200, engine.Health()); return; }
        if (method == "GET" && path.rfind("/v1/jobs/", 0) == 0) { Reply(socket, 200, engine.Job(path.substr(9))); return; }
        if (method == "GET" && path.rfind("/v1/traces/", 0) == 0) { Reply(socket, 200, engine.Trace(path.substr(11))); return; }
        if (method == "GET" && path.rfind("/v1/traces", 0) == 0) { size_t limit = 100; const auto marker = path.find("limit="); if (marker != std::string::npos) limit = std::stoull(path.substr(marker + 6)); Reply(socket, 200, {{"traces", engine.RecentTraces(std::min<size_t>(limit, 1000))}}); return; }
        const json input = body.empty() ? json::object() : json::parse(body);
        if (method == "POST" && path == "/v1/documents") Reply(socket, 200, engine.Ingest(input));
        else if (method == "POST" && path == "/v1/retrieve") Reply(socket, 200, engine.Retrieve(input));
        else if (method == "POST" && path == "/v1/evaluations") Reply(socket, 200, engine.Evaluate(input));
        else if (method == "POST" && path == "/v1/answers") {
            if (input.value("stream", false)) { StartSse(socket); try { auto result = engine.AnswerStream(input, [&](const std::string& token) { SendAll(socket, "data: " + json({{"type", "token"}, {"token", token}}).dump() + "\n\n"); }); SendAll(socket, "data: " + json({{"type", "complete"}, {"trace_id", result["trace_id"]}, {"citations", result["citations"]}}).dump() + "\n\ndata: [DONE]\n\n"); } catch (const std::exception& error) { SendAll(socket, "data: " + json({{"type", "error"}, {"error", error.what()}}).dump() + "\n\ndata: [DONE]\n\n"); } }
            else Reply(socket, 200, engine.Answer(input));
        }
        else if (method == "POST" && path == "/v1/chat/completions") {
            std::string query; for (auto it = input.at("messages").rbegin(); it != input.at("messages").rend(); ++it) if (it->value("role", "") == "user") { query = it->value("content", ""); break; }
            json options = json::object(); for (const auto& key : {"max_tokens", "temperature", "top_p", "stop"}) if (input.contains(key)) options[key] = input[key];
            json adapted = {{"query", query}, {"tenant_id", input.value("tenant_id", "default")}, {"access_scope", input.value("access_scope", "public")}, {"top_k", input.value("top_k", engine.GetConfig().server.top_k)}, {"options", options}, {"conversation", input.at("messages")}};
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
    int reuse = 1; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(engine_.GetConfig().server.port);
    if (inet_pton(AF_INET, engine_.GetConfig().server.host.c_str(), &address.sin_addr) != 1) throw std::runtime_error("invalid IPv4 bind address");
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 128) != 0) throw std::runtime_error("bind/listen failed: " + std::string(std::strerror(errno)));
    std::cout << "mimicrag_server listening on " << engine_.GetConfig().server.host << ':' << engine_.GetConfig().server.port << "\n";
    std::mutex queue_mutex;
    std::condition_variable queue_ready;
    std::deque<int> queue;
    const size_t workers = engine_.GetConfig().server.worker_threads ? engine_.GetConfig().server.worker_threads : std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (size_t i = 0; i < workers; ++i) pool.emplace_back([&] {
        for (;;) {
            int client;
            { std::unique_lock lock(queue_mutex); queue_ready.wait(lock, [&] { return !queue.empty(); }); client = queue.front(); queue.pop_front(); }
            Handle(client, engine_);
        }
    });
    for (;;) {
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) continue;
        std::lock_guard lock(queue_mutex);
        if (queue.size() >= 1024) { Reply(client, 503, {{"error", "server busy"}}); ::close(client); }
        else { queue.push_back(client); queue_ready.notify_one(); }
    }
}
}  // namespace mimicrag
