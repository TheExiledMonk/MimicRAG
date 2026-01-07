#ifndef PCDB_OUTPUT_COMPACTION_H
#define PCDB_OUTPUT_COMPACTION_H

#include <cstddef>
#include <vector>

#include "pcdb/mask.h"

namespace pcdb {

void CompactRowIdsPrefixSum(const Mask& mask, size_t count, std::vector<size_t>* out);

}  // namespace pcdb

#endif  // PCDB_OUTPUT_COMPACTION_H
