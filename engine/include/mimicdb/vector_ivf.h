#ifndef MIMICDB_VECTOR_IVF_H
#define MIMICDB_VECTOR_IVF_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mimicdb/vector_search.h"

namespace mimicdb {

class Dataset;

struct IvfSearchStats {
    size_t indexed_rows = 0;
    size_t centroid_count = 0;
    size_t probes = 0;
    size_t candidates = 0;
    size_t routing_dimensions = 0;
    uint64_t builds = 0;
    size_t shortlisted = 0;
    size_t lists_pruned = 0;
    double routing_seconds = 0.0;
    double shortlist_seconds = 0.0;
    double rerank_seconds = 0.0;
    double build_seconds = 0.0;
};

bool BuildVectorIvf(const Dataset& dataset, size_t field_index, VectorMetric metric);
bool VectorSearchIvf(const Dataset& dataset, size_t field_index, const float* query,
                     size_t dimension, size_t top_k, VectorMetric metric,
                     size_t probes, std::vector<VectorSearchHit>* out,
                     const std::vector<VectorSearchPredicate>& predicates = {});
IvfSearchStats GetVectorIvfStats(const Dataset& dataset, size_t field_index,
                                 VectorMetric metric);
void ReleaseVectorIvfDataset(const Dataset& dataset);

}  // namespace mimicdb

#endif
