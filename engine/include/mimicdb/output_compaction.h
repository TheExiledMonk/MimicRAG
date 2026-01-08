#ifndef MIMICDB_OUTPUT_COMPACTION_H
#define MIMICDB_OUTPUT_COMPACTION_H

#include <cstddef>
#include <vector>

#include "mimicdb/mask.h"

namespace mimicdb {

void CompactRowIdsPrefixSum(const Mask& mask, size_t count, std::vector<size_t>* out);

}  // namespace mimicdb

#endif  // MIMICDB_OUTPUT_COMPACTION_H
