#ifndef PCDB_SEGMENT_H
#define PCDB_SEGMENT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pcdb/field_vector.h"

namespace pcdb {

struct SegmentFormat {
    size_t row_capacity = 0;
};

struct SegmentColumnStats {
    double min = 0.0;
    double max = 0.0;
    uint64_t null_count = 0;
    bool has_value = false;
};

class Segment {
public:
    Segment(size_t row_capacity, std::vector<FieldVector> fields);
    Segment(size_t row_capacity, size_t row_count, std::vector<FieldVector> fields);

    size_t RowCapacity() const;
    size_t RowCount() const;
    bool Append(size_t rows);
    const SegmentFormat& Format() const;
    uint64_t SchemaFingerprint() const;
    const std::vector<FieldVector>& Fields() const;
    bool IsSealed() const;
    const std::vector<SegmentColumnStats>& ColumnStats() const;

private:
    size_t row_capacity_ = 0;
    size_t row_count_ = 0;
    std::vector<FieldVector> fields_;
    SegmentFormat format_;
    bool sealed_ = false;
    std::vector<SegmentColumnStats> stats_;

    void ComputeStats();
};

}  // namespace pcdb

#endif  // PCDB_SEGMENT_H
