#ifndef MIMICDB_VECTOR_SEARCH_H
#define MIMICDB_VECTOR_SEARCH_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mimicdb {

class Dataset;

enum class VectorMetric : uint8_t { kCosine = 0, kDot = 1, kL2Squared = 2 };

struct VectorSearchHit {
    uint64_t row_id = 0;
    float distance = 0.0F;
};

float VectorDistance(const float* left, const float* right, size_t dimension,
                     VectorMetric metric);
bool VectorSearch(const Dataset& dataset, size_t field_index, const float* query,
                  size_t dimension, size_t top_k, VectorMetric metric,
                  std::vector<VectorSearchHit>* out);

}  // namespace mimicdb

#endif
