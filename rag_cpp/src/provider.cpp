#include "mimicrag/provider.h"

#include <curl/curl.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace mimicrag {
namespace {
size_t WriteString(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

std::string JoinUrl(const std::string& base, const std::string& path) {
    if (base.empty()) throw std::runtime_error("provider base_url is empty");
    return base.back() == '/' ? base.substr(0, base.size() - 1) + path : base + path;
}

struct CurlHeaders {
    curl_slist* value = nullptr;
    ~CurlHeaders() { if (value) curl_slist_free_all(value); }
    void Add(const std::string& header) { value = curl_slist_append(value, header.c_str()); }
};

struct StreamState {
    std::string provider;
    std::string pending;
    std::string output;
    std::function<void(const std::string&)> emit;
};

void ParseStreamLine(StreamState& state, std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("data:", 0) == 0) { line.erase(0, 5); while (!line.empty() && line.front() == ' ') line.erase(line.begin()); }
    if (line.empty() || line == "[DONE]" || line.front() == ':') return;
    try {
        const auto event = nlohmann::json::parse(line);
        std::string token;
        if (state.provider == "anthropic" && event.value("type", "") == "content_block_delta") token = event.value("delta", nlohmann::json::object()).value("text", "");
        else if (state.provider == "ollama") token = event.value("message", nlohmann::json::object()).value("content", "");
        else if (state.provider == "cohere") token = event.value("delta", nlohmann::json::object()).value("message", nlohmann::json::object()).value("content", nlohmann::json::object()).value("text", "");
        else if (state.provider == "google") {
            if (event.contains("candidates")) for (const auto& part : event["candidates"][0]["content"]["parts"]) token += part.value("text", "");
        } else if (event.contains("choices") && !event["choices"].empty()) token = event["choices"][0].value("delta", nlohmann::json::object()).value("content", "");
        if (!token.empty()) { state.output += token; state.emit(token); }
    } catch (const std::exception&) {}
}

size_t WriteStream(char* data, size_t size, size_t count, void* user) {
    auto& state = *static_cast<StreamState*>(user);
    state.pending.append(data, size * count);
    size_t newline;
    while ((newline = state.pending.find('\n')) != std::string::npos) {
        ParseStreamLine(state, state.pending.substr(0, newline));
        state.pending.erase(0, newline + 1);
    }
    return size * count;
}
}  // namespace

RemoteProvider::RemoteProvider(ModelConfig config) : config_(std::move(config)) {
    static const int initialized = [] { curl_global_init(CURL_GLOBAL_DEFAULT); return 1; }();
    (void) initialized;
}

nlohmann::json RemoteProvider::PostJson(const std::string& path, const nlohmann::json& body) const {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl initialization failed");
    std::string response;
    const std::string payload = body.dump();
    CurlHeaders headers;
    headers.Add("Content-Type: application/json");
    for (const auto& item : config_.headers) headers.Add(item.first + ": " + item.second);
    const auto key = ResolveApiKey(config_);
    if (config_.provider == "anthropic") {
        headers.Add("x-api-key: " + key);
        headers.Add("anthropic-version: 2023-06-01");
    } else if (config_.provider == "azure_openai") {
        headers.Add("api-key: " + key);
    } else if (!key.empty()) {
        headers.Add("Authorization: Bearer " + key);
    }
    std::string request_path = path;
    if (config_.provider == "azure_openai") request_path += "?api-version=" + (config_.api_version.empty() ? "2024-10-21" : config_.api_version);
    const std::string url = JoinUrl(config_.base_url, request_path);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.value);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout_seconds);
    const CURLcode status = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);
    if (status != CURLE_OK || http_status < 200 || http_status >= 300) {
        throw std::runtime_error("provider request failed: HTTP " + std::to_string(http_status) + " " + response.substr(0, 512));
    }
    return nlohmann::json::parse(response);
}

std::string RemoteProvider::StreamJson(const std::string& path, const nlohmann::json& body,
                                       const std::function<void(const std::string&)>& stream) const {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl initialization failed");
    const std::string payload = body.dump();
    CurlHeaders headers; headers.Add("Content-Type: application/json"); headers.Add("Accept: text/event-stream");
    for (const auto& item : config_.headers) headers.Add(item.first + ": " + item.second);
    const auto key = ResolveApiKey(config_);
    if (config_.provider == "anthropic") { headers.Add("x-api-key: " + key); headers.Add("anthropic-version: 2023-06-01"); }
    else if (config_.provider == "azure_openai") headers.Add("api-key: " + key);
    else if (!key.empty()) headers.Add("Authorization: Bearer " + key);
    std::string request_path = path;
    if (config_.provider == "azure_openai") request_path += "?api-version=" + (config_.api_version.empty() ? "2024-10-21" : config_.api_version);
    StreamState state{config_.provider, {}, {}, stream};
    const std::string url = JoinUrl(config_.base_url, request_path);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str()); curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size())); curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.value);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStream); curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout_seconds);
    const CURLcode status = curl_easy_perform(curl); long http_status = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status); curl_easy_cleanup(curl);
    if (!state.pending.empty()) ParseStreamLine(state, state.pending);
    if (status != CURLE_OK || http_status < 200 || http_status >= 300) throw std::runtime_error("provider stream failed: HTTP " + std::to_string(http_status));
    return state.output;
}

std::vector<std::vector<float>> RemoteProvider::Embed(const std::vector<std::string>& texts, bool query) const {
    nlohmann::json response;
    if (config_.provider == "ollama") {
        response = PostJson("/api/embed", {{"model", config_.model}, {"input", texts}});
        return response.at("embeddings").get<std::vector<std::vector<float>>>();
    }
    if (config_.provider == "cohere") {
        response = PostJson("/embed", {{"model", config_.model}, {"texts", texts}, {"input_type", query ? "search_query" : "search_document"}, {"embedding_types", {"float"}}});
        return response.at("embeddings").at("float").get<std::vector<std::vector<float>>>();
    }
    if (config_.provider == "google") {
        nlohmann::json requests = nlohmann::json::array();
        for (const auto& text : texts) requests.push_back({{"model", "models/" + config_.model}, {"content", {{"parts", nlohmann::json::array({{{"text", text}}})}}}, {"taskType", query ? "RETRIEVAL_QUERY" : "RETRIEVAL_DOCUMENT"}});
        const auto key = ResolveApiKey(config_);
        response = PostJson("/models/" + config_.model + ":batchEmbedContents?key=" + key, {{"requests", requests}});
        std::vector<std::vector<float>> result; for (const auto& item : response.at("embeddings")) result.push_back(item.at("values").get<std::vector<float>>()); return result;
    }
    if (config_.provider == "anthropic") throw std::runtime_error("Anthropic has no embeddings API; enable local_embedding");
    nlohmann::json body = {{"input", texts}};
    if (config_.provider != "azure_openai") body["model"] = config_.model;
    response = PostJson("/embeddings", body);
    auto data = response.at("data");
    std::sort(data.begin(), data.end(), [](const auto& a, const auto& b) { return a.value("index", 0) < b.value("index", 0); });
    std::vector<std::vector<float>> result;
    for (const auto& item : data) result.push_back(item.at("embedding").get<std::vector<float>>());
    return result;
}

std::string RemoteProvider::Chat(const nlohmann::json& messages, const nlohmann::json& options,
                                 const std::function<void(const std::string&)>& stream) const {
    if (config_.provider == "anthropic") {
        std::string system;
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& message : messages) {
            if (message.at("role") == "system") system += message.at("content").get<std::string>();
            else filtered.push_back(message);
        }
        auto body = nlohmann::json{{"model", config_.model}, {"messages", filtered}, {"max_tokens", options.value("max_tokens", 1024)}};
        for (const auto& key : {"temperature", "top_p", "stop"}) if (options.contains(key)) body[key] = options[key];
        if (!system.empty()) body["system"] = system;
        if (stream) { body["stream"] = true; return StreamJson("/messages", body, stream); }
        const auto response = PostJson("/messages", body);
        std::string output;
        for (const auto& block : response.at("content")) if (block.value("type", "") == "text") output += block.value("text", "");
        return output;
    }
    if (config_.provider == "ollama") { auto body = nlohmann::json{{"model", config_.model}, {"messages", messages}, {"stream", bool(stream)}}; if (stream) return StreamJson("/api/chat", body, stream); return PostJson("/api/chat", body).at("message").at("content"); }
    if (config_.provider == "cohere") { auto body = nlohmann::json{{"model", config_.model}, {"messages", messages}}; for (const auto& key : {"max_tokens", "temperature", "top_p", "stop"}) if (options.contains(key)) body[key] = options[key]; if (stream) { body["stream"] = true; return StreamJson("/chat", body, stream); } return PostJson("/chat", body).at("message").at("content").at(0).at("text"); }
    if (config_.provider == "google") {
        nlohmann::json contents = nlohmann::json::array();
        for (const auto& message : messages) contents.push_back({{"role", message.at("role") == "assistant" ? "model" : "user"}, {"parts", nlohmann::json::array({{{"text", message.at("content")}}})}});
        nlohmann::json body = {{"contents", contents}};
        if (stream) return StreamJson("/models/" + config_.model + ":streamGenerateContent?alt=sse&key=" + ResolveApiKey(config_), body, stream);
        const auto response = PostJson("/models/" + config_.model + ":generateContent?key=" + ResolveApiKey(config_), body);
        std::string output; for (const auto& part : response.at("candidates").at(0).at("content").at("parts")) output += part.value("text", ""); return output;
    }
    nlohmann::json body = {{"messages", messages}}; for (const auto& key : {"max_tokens", "temperature", "top_p", "stop"}) if (options.contains(key)) body[key] = options[key];
    if (config_.provider != "azure_openai") body["model"] = config_.model;
    if (stream) { body["stream"] = true; return StreamJson("/chat/completions", body, stream); }
    return PostJson("/chat/completions", body).at("choices").at(0).at("message").at("content");
}

std::string RemoteProvider::Identity() const {
    return config_.provider + "\n" + config_.base_url + "\n" + config_.model;
}
}  // namespace mimicrag
