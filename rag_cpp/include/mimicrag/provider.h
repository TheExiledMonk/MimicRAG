#pragma once

#include "mimicrag/config.h"
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mimicrag {

class RemoteProvider {
public:
    explicit RemoteProvider(ModelConfig config);
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, bool query = false) const;
    std::string Chat(const nlohmann::json& messages, const nlohmann::json& options = {},
                     const std::function<void(const std::string&)>& stream = {}) const;
    std::string Identity() const;
private:
    ModelConfig config_;
    nlohmann::json PostJson(const std::string& path, const nlohmann::json& body) const;
    std::string StreamJson(const std::string& path, const nlohmann::json& body,
                           const std::function<void(const std::string&)>& stream) const;
};

}  // namespace mimicrag
