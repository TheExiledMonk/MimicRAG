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

std::string RemoteProvider::Chat(const nlohmann::json& messages, const std::function<void(const std::string&)>& stream) const {
    (void) stream;
    if (config_.provider == "anthropic") {
        std::string system;
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& message : messages) {
            if (message.at("role") == "system") system += message.at("content").get<std::string>();
            else filtered.push_back(message);
        }
        auto body = nlohmann::json{{"model", config_.model}, {"messages", filtered}, {"max_tokens", 1024}};
        if (!system.empty()) body["system"] = system;
        const auto response = PostJson("/messages", body);
        std::string output;
        for (const auto& block : response.at("content")) if (block.value("type", "") == "text") output += block.value("text", "");
        return output;
    }
    if (config_.provider == "ollama") return PostJson("/api/chat", {{"model", config_.model}, {"messages", messages}, {"stream", false}}).at("message").at("content");
    if (config_.provider == "cohere") return PostJson("/chat", {{"model", config_.model}, {"messages", messages}}).at("message").at("content").at(0).at("text");
    if (config_.provider == "google") {
        nlohmann::json contents = nlohmann::json::array();
        for (const auto& message : messages) contents.push_back({{"role", message.at("role") == "assistant" ? "model" : "user"}, {"parts", nlohmann::json::array({{{"text", message.at("content")}}})}});
        const auto response = PostJson("/models/" + config_.model + ":generateContent?key=" + ResolveApiKey(config_), {{"contents", contents}});
        std::string output; for (const auto& part : response.at("candidates").at(0).at("content").at("parts")) output += part.value("text", ""); return output;
    }
    nlohmann::json body = {{"messages", messages}};
    if (config_.provider != "azure_openai") body["model"] = config_.model;
    return PostJson("/chat/completions", body).at("choices").at(0).at("message").at("content");
}

std::string RemoteProvider::Identity() const {
    return config_.provider + "\n" + config_.base_url + "\n" + config_.model;
}
}  // namespace mimicrag
