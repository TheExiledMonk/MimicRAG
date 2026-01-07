#include "pcdb/metrics.h"

namespace pcdb {

void Metrics::Reset() {
    rows = 0;
    bytes = 0;
    branch_misses = 0;
    cache_misses = 0;
}

void Metrics::AddRows(uint64_t count) {
    rows += count;
}

void Metrics::AddBytes(uint64_t count) {
    bytes += count;
}

void Metrics::AddSegmentsTotal(uint64_t count) {
    segments_total += count;
}

void Metrics::AddSegmentsScanned(uint64_t count) {
    segments_scanned += count;
}

void Metrics::AddSegmentsPruned(uint64_t count) {
    segments_pruned += count;
}

}  // namespace pcdb
