#pragma once
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace mimicrag {
class Client {
 public:
  explicit Client(std::string base_url, std::string api_key = {}) : base_(std::move(base_url)), key_(std::move(api_key)) {}
  nlohmann::json Retrieve(const std::string& query, const std::string& tenant = "default", size_t top_k = 5) const {
    return Post("/v1/retrieve", {{"query", query}, {"tenant_id", tenant}, {"top_k", top_k}});
  }
  nlohmann::json Answer(const std::string& query, const std::string& tenant = "default") const {
    return Post("/v1/answers", {{"query", query}, {"tenant_id", tenant}});
  }
  nlohmann::json Ingest(const std::string& text, const std::string& uri, const std::string& tenant = "default") const {
    return Post("/v1/documents", {{"text", text}, {"source_uri", uri}, {"tenant_id", tenant}});
  }
 private:
  static size_t Write(char* data, size_t size, size_t count, void* target) { static_cast<std::string*>(target)->append(data, size * count); return size * count; }
  nlohmann::json Post(const std::string& path, const nlohmann::json& body) const {
    CURL* curl = curl_easy_init(); if (!curl) throw std::runtime_error("curl initialization failed");
    std::string response, encoded = body.dump(); struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!key_.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + key_).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, (base_ + path).c_str()); curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encoded.c_str()); curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Write); curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const CURLcode status = curl_easy_perform(curl); long http = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    if (status != CURLE_OK || http >= 400) throw std::runtime_error("MimicRAG request failed: " + response);
    return nlohmann::json::parse(response);
  }
  std::string base_, key_;
};
}  // namespace mimicrag
