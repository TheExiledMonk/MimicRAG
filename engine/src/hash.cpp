#include "mimicdb/hash.h"

namespace mimicdb {

namespace {
constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
}

uint64_t HashBytes(uint64_t hash, const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

uint64_t HashString(uint64_t hash, const std::string& value) {
    return HashBytes(hash, value.data(), value.size());
}

uint64_t HashValue(uint64_t hash, const void* data, size_t length) {
    return HashBytes(hash, data, length);
}

uint64_t HashInit() {
    return kFnvOffsetBasis;
}

}  // namespace mimicdb
