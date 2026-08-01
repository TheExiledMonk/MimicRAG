#ifndef MIMICDB_VECTOR_SEARCH_H
#define MIMICDB_VECTOR_SEARCH_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include "mimicdb/predicate.h"

namespace mimicdb {

class Dataset;

enum class VectorMetric : uint8_t { kCosine = 0, kDot = 1, kL2Squared = 2 };

struct VectorSearchHit {
    uint64_t row_id = 0;
    float distance = 0.0F;
};

struct VectorSearchPredicate {
    size_t field_index = 0;
    CompareOp op = CompareOp::kEq;
    double value = 0.0;
};

struct VectorSearchRuntimeStats {
    size_t cpu_threads = 1;
    size_t gpu_crossover_elements = 0;
    size_t calibration_max_elements = 0;
    bool gpu_calibrated = false;
};

float VectorDistance(const float* left, const float* right, size_t dimension,
                     VectorMetric metric);
bool VectorSearch(const Dataset& dataset, size_t field_index, const float* query,
                  size_t dimension, size_t top_k, VectorMetric metric,
                  std::vector<VectorSearchHit>* out,
                  const std::vector<VectorSearchPredicate>& predicates = {});
VectorSearchRuntimeStats GetVectorSearchRuntimeStats();
// Call before the first vector search; zero selects hardware concurrency.
void ConfigureVectorSearchThreads(size_t threads);

}  // namespace mimicdb

#endif
