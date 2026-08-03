#pragma once

#include "mimicrag/config.h"
#include <filesystem>
#include <nlohmann/json.hpp>

namespace mimicrag {

nlohmann::json CreateSnapshot(const Config& config, const std::filesystem::path& destination);
nlohmann::json VerifySnapshot(const std::filesystem::path& snapshot);
nlohmann::json RestoreSnapshot(const std::filesystem::path& snapshot,
                               const std::filesystem::path& destination, bool rehearsal);
nlohmann::json Doctor(const Config& config);
nlohmann::json RepairDerivedIndexes(const Config& config);

}  // namespace mimicrag
