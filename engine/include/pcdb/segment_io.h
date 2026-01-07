#ifndef PCDB_SEGMENT_IO_H
#define PCDB_SEGMENT_IO_H

#include <cstdint>
#include <string>
#include <vector>

#include "pcdb/segment.h"

namespace pcdb {

class Dataset;
class Schema;

struct SegmentHeader {
    uint32_t magic = 0x50434442;  // 'PCDB'
    uint32_t version = 1;
    uint64_t row_capacity = 0;
    uint64_t row_count = 0;
    uint64_t schema_fingerprint = 0;
    uint32_t field_count = 0;
    uint32_t reserved = 0;
};

struct SegmentColumnHeader {
    uint32_t type = 0;
    uint32_t reserved = 0;
    uint64_t value_count = 0;
    uint64_t data_bytes = 0;
    uint64_t validity_words = 0;
    double min = 0.0;
    double max = 0.0;
    uint64_t null_count = 0;
    uint8_t has_value = 0;
    uint8_t padding[7] = {};
};

class SegmentWriter {
public:
    SegmentWriter() = default;
    explicit SegmentWriter(std::string path);

    bool Open(std::string path);
    bool Write(const Segment& segment);
    const std::string& Path() const;
    bool IsOpen() const;
    const SegmentHeader& Header() const;

private:
    std::string path_;
    bool is_open_ = false;
    SegmentHeader header_{};
};

class SegmentReader {
public:
    SegmentReader() = default;
    explicit SegmentReader(std::string path);

    bool Open(std::string path);
    bool Read(Segment* out_segment);
    bool ReadWithSchema(Segment* out_segment, const Schema& schema);
    void SetExpectedSchemaFingerprint(uint64_t fingerprint);
    const std::string& Path() const;
    bool IsOpen() const;
    const SegmentHeader& Header() const;
    const std::vector<SegmentColumnHeader>& ColumnHeaders() const;

private:
    std::string path_;
    bool is_open_ = false;
    SegmentHeader header_{};
    uint64_t expected_schema_fingerprint_ = 0;
    std::vector<SegmentColumnHeader> column_headers_;
};

}  // namespace pcdb

#endif  // PCDB_SEGMENT_IO_H
