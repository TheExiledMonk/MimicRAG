#pragma once

#include "mimicrag/config.h"
#include <memory>
#include <string>
#include <vector>

namespace mimicrag {

class LocalEmbedder {
public:
    explicit LocalEmbedder(const LocalEmbeddingConfig& config);
    ~LocalEmbedder();
    LocalEmbedder(const LocalEmbedder&) = delete;
    LocalEmbedder& operator=(const LocalEmbedder&) = delete;
    bool Available() const;
    bool UsingGpu() const;
    size_t Dimension() const;
    std::string Identity() const;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, bool query = false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mimicrag
