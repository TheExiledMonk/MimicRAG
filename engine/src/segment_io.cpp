#include "pcdb/segment_io.h"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "pcdb/schema.h"

namespace pcdb {

SegmentWriter::SegmentWriter(std::string path) {
    Open(std::move(path));
}

bool SegmentWriter::Open(std::string path) {
    path_ = std::move(path);
    is_open_ = !path_.empty();
    return is_open_;
}

bool SegmentWriter::Write(const Segment& segment) {
    if (!is_open_) {
        return false;
    }
    {
        std::ifstream existing(path_, std::ios::binary);
        if (existing.good()) {
            return false;
        }
    }
    header_.row_capacity = segment.RowCapacity();
    header_.row_count = segment.RowCount();
    header_.schema_fingerprint = segment.SchemaFingerprint();
    header_.field_count = static_cast<uint32_t>(segment.Fields().size());
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
    if (!out.good()) {
        return false;
    }

    std::vector<SegmentColumnHeader> columns;
    columns.reserve(segment.Fields().size());
    const auto& stats = segment.ColumnStats();
    for (size_t i = 0; i < segment.Fields().size(); ++i) {
        const auto& field = segment.Fields()[i];
        if (field.Size() != header_.row_count) {
            return false;
        }
        SegmentColumnHeader col;
        col.type = static_cast<uint32_t>(field.Type());
        col.value_count = field.Size();
        switch (field.Type()) {
            case FieldType::kInt32:
                col.data_bytes = field.Size() * sizeof(int32_t);
                break;
            case FieldType::kInt64:
                col.data_bytes = field.Size() * sizeof(int64_t);
                break;
            case FieldType::kFloat64:
                col.data_bytes = field.Size() * sizeof(double);
                break;
            case FieldType::kBool:
                col.data_bytes = field.Size() * sizeof(uint8_t);
                break;
            case FieldType::kDictInt32:
                col.data_bytes = field.Size() * sizeof(uint32_t);
                break;
        }
        col.validity_words = field.HasNulls() ? field.Validity().WordCount() : 0;
        if (i < stats.size()) {
            col.min = stats[i].min;
            col.max = stats[i].max;
            col.null_count = stats[i].null_count;
            col.has_value = stats[i].has_value ? 1 : 0;
        }
        columns.push_back(col);
    }

    if (!columns.empty()) {
        out.write(reinterpret_cast<const char*>(columns.data()),
                  columns.size() * sizeof(SegmentColumnHeader));
        if (!out.good()) {
            return false;
        }
    }

    for (size_t i = 0; i < columns.size(); ++i) {
        const auto& field = segment.Fields()[i];
        switch (field.Type()) {
            case FieldType::kInt32:
                out.write(reinterpret_cast<const char*>(field.DataInt32()), columns[i].data_bytes);
                break;
            case FieldType::kInt64:
                out.write(reinterpret_cast<const char*>(field.DataInt64()), columns[i].data_bytes);
                break;
            case FieldType::kFloat64:
                out.write(reinterpret_cast<const char*>(field.DataFloat64()), columns[i].data_bytes);
                break;
            case FieldType::kBool:
                out.write(reinterpret_cast<const char*>(field.DataBool()), columns[i].data_bytes);
                break;
            case FieldType::kDictInt32:
                out.write(reinterpret_cast<const char*>(field.DataDictIds()), columns[i].data_bytes);
                break;
        }
        if (!out.good()) {
            return false;
        }
    }

    for (size_t i = 0; i < columns.size(); ++i) {
        const auto& field = segment.Fields()[i];
        if (columns[i].validity_words == 0) {
            continue;
        }
        out.write(reinterpret_cast<const char*>(field.Validity().Words()),
                  columns[i].validity_words * sizeof(uint64_t));
        if (!out.good()) {
            return false;
        }
    }

    return out.good();
}

const std::string& SegmentWriter::Path() const {
    return path_;
}

bool SegmentWriter::IsOpen() const {
    return is_open_;
}

const SegmentHeader& SegmentWriter::Header() const {
    return header_;
}

SegmentReader::SegmentReader(std::string path) {
    Open(std::move(path));
}

bool SegmentReader::Open(std::string path) {
    path_ = std::move(path);
    is_open_ = !path_.empty();
    return is_open_;
}

bool SegmentReader::Read(Segment* out_segment) {
    if (!is_open_) {
        return false;
    }
    column_headers_.clear();
    const uint8_t* base = nullptr;
    size_t mapped_size = 0;
#if defined(__unix__) || defined(__APPLE__)
    int fd = open(path_.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < sizeof(header_)) {
        close(fd);
        return false;
    }
    mapped_size = static_cast<size_t>(st.st_size);
    void* mapped = mmap(nullptr, mapped_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return false;
    }
    base = static_cast<const uint8_t*>(mapped);
    std::memcpy(&header_, base, sizeof(header_));
    close(fd);
#else
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    in.seekg(0, std::ios::end);
    mapped_size = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(mapped_size);
    in.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    if (!in.good()) {
        return false;
    }
    base = buffer.data();
    std::memcpy(&header_, base, sizeof(header_));
#endif
    if (header_.magic != 0x50434442 || header_.version != 1) {
#if defined(__unix__) || defined(__APPLE__)
        munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
        return false;
    }
    if (expected_schema_fingerprint_ != 0 &&
        header_.schema_fingerprint != expected_schema_fingerprint_) {
#if defined(__unix__) || defined(__APPLE__)
        munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
        return false;
    }
    const size_t header_bytes =
        sizeof(SegmentHeader) + static_cast<size_t>(header_.field_count) * sizeof(SegmentColumnHeader);
    if (mapped_size < header_bytes) {
#if defined(__unix__) || defined(__APPLE__)
        munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
        return false;
    }
    const auto* column_headers = reinterpret_cast<const SegmentColumnHeader*>(base + sizeof(SegmentHeader));
    column_headers_.assign(column_headers, column_headers + header_.field_count);
    size_t offset = header_bytes;

    std::vector<FieldVector> fields;
    fields.reserve(header_.field_count);
    for (uint32_t i = 0; i < header_.field_count; ++i) {
        const auto& col = column_headers_[i];
        if (col.value_count != header_.row_count) {
#if defined(__unix__) || defined(__APPLE__)
            munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
            return false;
        }
        if (offset + col.data_bytes > mapped_size) {
#if defined(__unix__) || defined(__APPLE__)
            munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
            return false;
        }
        FieldVector field("col" + std::to_string(i), static_cast<FieldType>(col.type));
        field.Resize(static_cast<size_t>(col.value_count));
        switch (static_cast<FieldType>(col.type)) {
            case FieldType::kInt32:
                std::memcpy(field.MutableInt32(), base + offset, col.data_bytes);
                break;
            case FieldType::kInt64:
                std::memcpy(field.MutableInt64(), base + offset, col.data_bytes);
                break;
            case FieldType::kFloat64:
                std::memcpy(field.MutableFloat64(), base + offset, col.data_bytes);
                break;
            case FieldType::kBool:
                std::memcpy(field.MutableBool(), base + offset, col.data_bytes);
                break;
            case FieldType::kDictInt32:
                std::memcpy(field.MutableDictIds(), base + offset, col.data_bytes);
                break;
        }
        offset += static_cast<size_t>(col.data_bytes);
        if (col.validity_words > 0) {
            const size_t validity_bytes = static_cast<size_t>(col.validity_words) * sizeof(uint64_t);
            if (offset + validity_bytes > mapped_size) {
#if defined(__unix__) || defined(__APPLE__)
                munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
                return false;
            }
            field.LoadValidityWords(
                reinterpret_cast<const uint64_t*>(base + offset),
                static_cast<size_t>(col.validity_words),
                static_cast<size_t>(col.value_count));
            offset += validity_bytes;
        }
        fields.push_back(std::move(field));
    }
    if (out_segment != nullptr) {
        Segment segment(static_cast<size_t>(header_.row_capacity),
                        static_cast<size_t>(header_.row_count),
                        std::move(fields));
        *out_segment = std::move(segment);
    }
#if defined(__unix__) || defined(__APPLE__)
    munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
    return true;
}

bool SegmentReader::ReadWithSchema(Segment* out_segment, const Schema& schema) {
    SetExpectedSchemaFingerprint(schema.Fingerprint());
    return Read(out_segment);
}

void SegmentReader::SetExpectedSchemaFingerprint(uint64_t fingerprint) {
    expected_schema_fingerprint_ = fingerprint;
}

const std::string& SegmentReader::Path() const {
    return path_;
}

bool SegmentReader::IsOpen() const {
    return is_open_;
}

const SegmentHeader& SegmentReader::Header() const {
    return header_;
}

const std::vector<SegmentColumnHeader>& SegmentReader::ColumnHeaders() const {
    return column_headers_;
}

}  // namespace pcdb
