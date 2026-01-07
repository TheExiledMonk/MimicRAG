#ifndef PCDB_SIMD_OUTPUT_H
#define PCDB_SIMD_OUTPUT_H

#include <cstddef>
#include <vector>

#include "pcdb/mask.h"

namespace pcdb {

void CompressStoreScalar(const Mask& mask, size_t count, std::vector<size_t>* out);

}  // namespace pcdb

#endif  // PCDB_SIMD_OUTPUT_H
