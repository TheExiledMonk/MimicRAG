#ifndef MIMICDB_SEGMENT_H
#define MIMICDB_SEGMENT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mimicdb/compression.h"
#include "mimicdb/field_vector.h"

namespace mimicdb {

struct SegmentFormat {
    size_t row_capacity = 0;
};

struct SegmentColumnStats {
    double min = 0.0;
    double max = 0.0;
    uint64_t value_count = 0;
    uint64_t null_count = 0;
    uint32_t estimated_cardinality = 0;
    uint8_t monotonic_hint = 0;  // 0 unknown, 1 non-decreasing, 2 non-increasing, 3 not monotonic
    bool has_value = false;
};

class Segment {
public:
    Segment(size_t row_capacity, std::vector<FieldVector> fields);
    Segment(size_t row_capacity, size_t row_count, std::vector<FieldVector> fields);
    Segment(size_t row_capacity, size_t row_count, std::vector<FieldVector> fields,
            bool build_compression);

    size_t RowCapacity() const;
    size_t RowCount() const;
    bool Append(size_t rows);
    const SegmentFormat& Format() const;
    uint64_t SchemaFingerprint() const;
    const std::vector<FieldVector>& Fields() const;
    bool IsSealed() const;
    const std::vector<SegmentColumnStats>& ColumnStats() const;
    const std::vector<ColumnCompressionKind>& CompressionKinds() const;
    void SetCompressionKinds(std::vector<ColumnCompressionKind> kinds);
    const std::vector<CompressedColumnView>& CompressedColumns() const;
    void ReleaseFieldValues(size_t field_index);
    bool FieldValuesResident(size_t field_index) const;

private:
    size_t row_capacity_ = 0;
    size_t row_count_ = 0;
    std::vector<FieldVector> fields_;
    SegmentFormat format_;
    bool sealed_ = false;
    std::vector<SegmentColumnStats> stats_;
    std::vector<ColumnCompressionKind> compression_kinds_;
    std::vector<CompressedColumnView> compressed_columns_;
    std::vector<std::vector<uint8_t>> compressed_data_;
    std::vector<std::vector<uint8_t>> compressed_aux_;
    std::vector<std::vector<uint64_t>> compressed_validity_;
    bool compression_ready_ = false;

    void ComputeStats(bool build_compression = true);
    void BuildCompressionViews();
    void ResetCompression();
};

}  // namespace mimicdb

#endif  // MIMICDB_SEGMENT_H
