#ifndef MIMICDB_CONFIG_H
#define MIMICDB_CONFIG_H

#include <cstddef>

namespace mimicdb {

struct Config {
    static constexpr size_t kDefaultSegmentRows = 4096;
    static constexpr size_t kDefaultBatchRows = 1024;
};

}  // namespace mimicdb

#endif  // MIMICDB_CONFIG_H
