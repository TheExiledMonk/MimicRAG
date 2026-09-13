#pragma once

#include <string>

namespace mimicdb {

// Returns the opaque ID that must appear in the configured license file.
std::string LicenseMachineId();

// Fetches the configured license file and checks for an exact, trimmed line.
bool HasMachineLicense(const std::string& storage_root, std::string* reason);

}  // namespace mimicdb
