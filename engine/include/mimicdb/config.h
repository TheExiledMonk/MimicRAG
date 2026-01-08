#ifndef MIMICDB_CONFIG_H
#define MIMICDB_CONFIG_H

#include <cstddef>

namespace mimicdb {

struct Config {
    static constexpr size_t kDefaultSegmentRows = 4096;
    static constexpr size_t kDefaultBatchRows = 1024;
    static constexpr uint32_t kCompressionDictCardinalityThreshold = 256;
    static constexpr uint64_t kCompressionForDeltaMaxRange = 1ULL << 20;
    static constexpr uint8_t kCompressionBitpackMaxBits = 16;
    static constexpr double kCompressionMinRatio = 1.1;
};

}  // namespace mimicdb

#endif  // MIMICDB_CONFIG_H
