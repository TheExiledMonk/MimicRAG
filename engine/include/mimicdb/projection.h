#ifndef MIMICDB_PROJECTION_H
#define MIMICDB_PROJECTION_H

#include <cstddef>
#include <vector>

#include "mimicdb/field_vector.h"

namespace mimicdb {

struct Projection {
    std::vector<size_t> indices;
};

struct ProjectionResult {
    std::vector<FieldVector> fields;
};

ProjectionResult ProjectRows(const std::vector<FieldVector>& fields,
                             const Projection& projection,
                             const std::vector<size_t>& row_ids);

}  // namespace mimicdb

#endif  // MIMICDB_PROJECTION_H
