#ifndef PCDB_CONFIG_H
#define PCDB_CONFIG_H

#include <cstddef>

namespace pcdb {

struct Config {
    static constexpr size_t kDefaultSegmentRows = 4096;
    static constexpr size_t kDefaultBatchRows = 1024;
};

}  // namespace pcdb

#endif  // PCDB_CONFIG_H
