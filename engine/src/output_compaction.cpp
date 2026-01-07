#include "pcdb/output_compaction.h"

namespace pcdb {

void CompactRowIdsPrefixSum(const Mask& mask, size_t count, std::vector<size_t>* out) {
    if (!out) {
        return;
    }
    out->clear();
    if (count == 0) {
        return;
    }
    std::vector<size_t> prefix(count + 1, 0);
    for (size_t i = 0; i < count; ++i) {
        prefix[i + 1] = prefix[i] + (mask.Get(i) ? 1U : 0U);
    }
    const size_t total = prefix[count];
    out->resize(total);
    for (size_t i = 0; i < count; ++i) {
        if (mask.Get(i)) {
            out->at(prefix[i]) = i;
        }
    }
}

}  // namespace pcdb
