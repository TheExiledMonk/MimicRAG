#ifndef MIMICDB_SIMD_OUTPUT_H
#define MIMICDB_SIMD_OUTPUT_H

#include <cstddef>
#include <vector>

#include "mimicdb/mask.h"

namespace mimicdb {

void CompressStoreScalar(const Mask& mask, size_t count, std::vector<size_t>* out);
void CompressStorePacked(const PackedMask& mask, size_t count, std::vector<size_t>* out);

}  // namespace mimicdb

#endif  // MIMICDB_SIMD_OUTPUT_H
