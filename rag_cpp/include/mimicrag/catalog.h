#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mimicrag {

struct CatalogRecord {
    nlohmann::json document;
    std::vector<std::vector<float>> remote_embeddings;
    std::vector<std::vector<float>> local_embeddings;
};

class BinaryCatalog {
public:
    explicit BinaryCatalog(std::filesystem::path path);
    bool Exists() const;
    void Append(const CatalogRecord& record);
    size_t Replay(const std::function<void(CatalogRecord&&)>& consume);
    nlohmann::json Inspect();
    nlohmann::json Compact();
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

}  // namespace mimicrag
