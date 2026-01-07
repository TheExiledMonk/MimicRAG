#ifndef PCDB_METRICS_H
#define PCDB_METRICS_H

#include <cstdint>

namespace pcdb {

struct Metrics {
    uint64_t rows = 0;
    uint64_t bytes = 0;
    uint64_t branch_misses = 0;
    uint64_t cache_misses = 0;
    uint64_t segments_total = 0;
    uint64_t segments_scanned = 0;
    uint64_t segments_pruned = 0;

    void Reset();
    void AddRows(uint64_t count);
    void AddBytes(uint64_t count);
    void AddSegmentsTotal(uint64_t count);
    void AddSegmentsScanned(uint64_t count);
    void AddSegmentsPruned(uint64_t count);
};

}  // namespace pcdb

#endif  // PCDB_METRICS_H
