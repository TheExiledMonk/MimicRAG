#pragma once

#include "mimicrag/config.h"
#include "mimicrag/local_embeddings.h"
#include "mimicrag/provider.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mimicrag {

class RagEngine {
public:
    explicit RagEngine(Config config);
    ~RagEngine();
    nlohmann::json Ingest(const nlohmann::json& request);
    nlohmann::json Retrieve(const nlohmann::json& request);
    nlohmann::json Answer(const nlohmann::json& request);
    nlohmann::json Health() const;
    const Config& GetConfig() const { return config_; }
private:
    struct Impl;
    Config config_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mimicrag
