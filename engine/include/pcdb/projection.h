#ifndef PCDB_PROJECTION_H
#define PCDB_PROJECTION_H

#include <cstddef>
#include <vector>

#include "pcdb/field_vector.h"

namespace pcdb {

struct Projection {
    std::vector<size_t> indices;
};

struct ProjectionResult {
    std::vector<FieldVector> fields;
};

ProjectionResult ProjectRows(const std::vector<FieldVector>& fields,
                             const Projection& projection,
                             const std::vector<size_t>& row_ids);

}  // namespace pcdb

#endif  // PCDB_PROJECTION_H
