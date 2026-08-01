#ifndef MIMICDB_VECTOR_GPU_H
#define MIMICDB_VECTOR_GPU_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mimicdb/vector_search.h"

namespace mimicdb {

class Dataset;

struct VectorGpuStats {
    bool available = false;
    const char* device_name = "unavailable";
    uint64_t resident_bytes = 0;
    uint64_t uploads = 0;
    uint64_t searches = 0;
};

// Uploads immutable sealed segments. Safe to call after every append; unchanged data is a no-op.
bool PreloadVectorField(const Dataset& dataset, size_t field_index);
void ReleaseVectorGpuDataset(const Dataset& dataset);
VectorGpuStats GetVectorGpuStats();

}  // namespace mimicdb

#endif
