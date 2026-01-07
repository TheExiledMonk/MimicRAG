#include "pcdb/simd_output.h"

namespace pcdb {

void CompressStoreScalar(const Mask& mask, size_t count, std::vector<size_t>* out) {
    if (!out) {
        return;
    }
    out->clear();
    for (size_t i = 0; i < count; ++i) {
        if (mask.Get(i)) {
            out->push_back(i);
        }
    }
}

}  // namespace pcdb
