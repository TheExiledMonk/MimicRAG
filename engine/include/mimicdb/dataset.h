#ifndef MIMICDB_DATASET_H
#define MIMICDB_DATASET_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "mimicdb/field_vector.h"
#include "mimicdb/schema.h"
#include "mimicdb/segment.h"

namespace mimicdb {

struct FieldValue {
    FieldType type;
    bool is_null = false;
    int32_t i32 = 0;
    int64_t i64 = 0;
    double f64 = 0.0;
    bool b = false;
    std::string bytes;
    std::vector<FieldValue> array;
    std::unordered_map<std::string, FieldValue> object;

    static FieldValue Int32(int32_t value);
    static FieldValue Int64(int64_t value);
    static FieldValue Float64(double value);
    static FieldValue Bool(bool value);
    static FieldValue String(const std::string& value);
    static FieldValue Bytes(const std::string& value);
    static FieldValue Array(const std::vector<FieldValue>& value);
    static FieldValue Object(const std::unordered_map<std::string, FieldValue>& value);
    static FieldValue Null(FieldType type);
};

struct FieldBatch {
    FieldType type;
    const void* data = nullptr;
    size_t count = 0;
    const uint8_t* validity = nullptr;
    const uint32_t* lengths = nullptr;
    const uint8_t* bytes = nullptr;
    size_t bytes_size = 0;
};

class Dataset {
public:
    explicit Dataset(std::string name);

    const std::string& Name() const;
    size_t RowCount() const;
    size_t SegmentCapacity() const;
    void AddField(FieldVector field);
    const std::vector<FieldVector>& Fields() const;
    const std::vector<FieldVector>& ActiveFields() const;
    size_t ActiveRowCount() const;
    const std::vector<Segment>& Segments() const;
    bool Append(const std::vector<FieldValue>& values);
    bool AppendBatch(const std::vector<FieldBatch>& batches);
    bool AddRecoveredSegment(Segment segment);
    void DropSegments(size_t count);
    uint64_t SchemaFingerprint() const;
    Schema SchemaView() const;

private:
    std::string name_;
    size_t rows_ = 0;
    std::vector<FieldVector> fields_;
    std::vector<FieldVector> active_fields_;
    std::vector<Segment> segments_;
    size_t segment_capacity_ = 0;
};

}  // namespace mimicdb

#endif  // MIMICDB_DATASET_H
