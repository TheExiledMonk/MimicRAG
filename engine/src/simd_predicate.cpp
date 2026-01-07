#include "pcdb/simd.h"

namespace pcdb {

namespace {
void PredicateInt64EqScalar(const int64_t* data, int64_t value, uint8_t* out, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        out[i] = static_cast<uint8_t>(data[i] == value);
    }
}
}  // namespace

PredicateKernelInt64 GetPredicateKernelInt64Eq() {
    return &PredicateInt64EqScalar;
}

}  // namespace pcdb
