#include "mimicdb/segment_io.h"

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

#include "mimicdb/compression.h"
#include "mimicdb/schema.h"

namespace mimicdb {

namespace {
bool FsyncPath(const std::string& path) {
#if defined(__unix__) || defined(__APPLE__)
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const int result = ::fsync(fd);
    ::close(fd);
    return result == 0;
#else
    (void)path;
    return true;
#endif
}
}  // namespace

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
    const auto& compressed = segment.CompressedColumns();
    const bool use_compressed = segment.IsSealed() && !compressed.empty();
    for (size_t i = 0; i < segment.Fields().size(); ++i) {
        const auto& field = segment.Fields()[i];
        if (field.Size() != header_.row_count) {
            return false;
        }
        const CompressedColumnView* view = nullptr;
        if (use_compressed && i < compressed.size()) {
            view = &compressed[i];
        }
        SegmentColumnHeader col;
        col.type = static_cast<uint32_t>(field.Type());
        col.value_count = field.Size();
        if (view) {
            col.data_bytes = view->data_size;
            col.aux_bytes = view->aux_size;
            col.validity_words = view->validity_word_count;
        } else {
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
                case FieldType::kString:
                case FieldType::kBytes:
                case FieldType::kArray:
                case FieldType::kVectorFloat32:
                    col.aux_bytes = field.Size() * sizeof(uint32_t);
                    col.data_bytes = field.BytesSize();
                    break;
                case FieldType::kObject:
                    return false;
            }
            col.validity_words = field.HasNulls() ? field.Validity().WordCount() : 0;
        }
        if (i < stats.size()) {
            col.min = stats[i].min;
            col.max = stats[i].max;
            col.null_count = stats[i].null_count;
            col.has_value = stats[i].has_value ? 1 : 0;
            col.estimated_cardinality = stats[i].estimated_cardinality;
            col.monotonic_hint = stats[i].monotonic_hint;
        }
        const auto& kinds = segment.CompressionKinds();
        if (i < kinds.size()) {
            col.compression_kind = static_cast<uint8_t>(kinds[i]);
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
        const CompressedColumnView* view = nullptr;
        if (use_compressed && i < compressed.size()) {
            view = &compressed[i];
        }
        if (view) {
            if (view->aux_size > 0 && view->aux) {
                out.write(reinterpret_cast<const char*>(view->aux), columns[i].aux_bytes);
            }
            if (view->data_size > 0 && view->data) {
                out.write(reinterpret_cast<const char*>(view->data), columns[i].data_bytes);
            }
        } else {
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
                case FieldType::kString:
                case FieldType::kBytes:
                case FieldType::kArray:
                case FieldType::kVectorFloat32:
                    out.write(reinterpret_cast<const char*>(field.DataLengths()), columns[i].aux_bytes);
                    out.write(reinterpret_cast<const char*>(field.DataBytes()), columns[i].data_bytes);
                    break;
                case FieldType::kObject:
                    return false;
            }
        }
        if (!out.good()) {
            return false;
        }
        if (columns[i].validity_words == 0) {
            continue;
        }
        if (view && view->validity_words) {
            out.write(reinterpret_cast<const char*>(view->validity_words),
                      columns[i].validity_words * sizeof(uint64_t));
        } else {
            out.write(reinterpret_cast<const char*>(field.Validity().Words()),
                      columns[i].validity_words * sizeof(uint64_t));
        }
        if (!out.good()) {
            return false;
        }
    }

    const bool ok = out.good();
    out.close();
    if (!ok) {
        return false;
    }
    return FsyncPath(path_);
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
    if (header_.magic != 0x4D434442 || header_.version != 1) {
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
        const size_t total_bytes =
            static_cast<size_t>(col.data_bytes + col.aux_bytes);
        if (offset + total_bytes > mapped_size) {
#if defined(__unix__) || defined(__APPLE__)
            munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
            return false;
        }
        FieldVector field("col" + std::to_string(i), static_cast<FieldType>(col.type));
        const auto field_type = static_cast<FieldType>(col.type);
        const bool is_varlen = field_type == FieldType::kString ||
            field_type == FieldType::kBytes || field_type == FieldType::kArray ||
            field_type == FieldType::kVectorFloat32;
        if (!is_varlen) {
            field.Resize(static_cast<size_t>(col.value_count));
        }
        ColumnCompressionKind kind =
            static_cast<ColumnCompressionKind>(col.compression_kind);
        if (field_type == FieldType::kFloat64 && kind != ColumnCompressionKind::kLz4) {
            kind = ColumnCompressionKind::kNone;
        }
        const size_t aux_bytes = static_cast<size_t>(col.aux_bytes);
        const uint8_t* aux_ptr = aux_bytes > 0 ? base + offset : nullptr;
        const uint8_t* data_ptr = base + offset + aux_bytes;
        if (kind == ColumnCompressionKind::kNone) {
            switch (field_type) {
                case FieldType::kInt32:
                    std::memcpy(field.MutableInt32(), data_ptr, col.data_bytes);
                    break;
                case FieldType::kInt64:
                    std::memcpy(field.MutableInt64(), data_ptr, col.data_bytes);
                    break;
                case FieldType::kFloat64:
                    std::memcpy(field.MutableFloat64(), data_ptr, col.data_bytes);
                    break;
                case FieldType::kBool:
                    std::memcpy(field.MutableBool(), data_ptr, col.data_bytes);
                    break;
                case FieldType::kDictInt32:
                    std::memcpy(field.MutableDictIds(), data_ptr, col.data_bytes);
                    break;
                case FieldType::kString:
                case FieldType::kBytes:
                case FieldType::kArray:
                case FieldType::kVectorFloat32: {
                    const size_t length_bytes = static_cast<size_t>(col.aux_bytes);
                    if (offset + length_bytes + col.data_bytes > mapped_size) {
#if defined(__unix__) || defined(__APPLE__)
                        munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
                        return false;
                    }
                    std::vector<uint32_t> lengths(static_cast<size_t>(col.value_count));
                    if (length_bytes > 0) {
                        std::memcpy(lengths.data(), aux_ptr, length_bytes);
                    }
                    const auto* bytes = reinterpret_cast<const uint8_t*>(data_ptr);
                    if (!field.LoadVarlen(lengths.data(), static_cast<size_t>(col.value_count),
                                          bytes, static_cast<size_t>(col.data_bytes))) {
#if defined(__unix__) || defined(__APPLE__)
                        munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
                        return false;
                    }
                    offset += length_bytes;
                    break;
                }
                case FieldType::kObject:
                    return false;
            }
        } else if (kind == ColumnCompressionKind::kLz4) {
                if (is_varlen) {
                    const size_t length_bytes = static_cast<size_t>(col.value_count) *
                        sizeof(uint32_t);
                    std::vector<uint8_t> decoded_lengths(length_bytes);
                if (!DecodeLz4Literal(aux_ptr, static_cast<size_t>(col.aux_bytes),
                                      decoded_lengths.data(), decoded_lengths.size())) {
#if defined(__unix__) || defined(__APPLE__)
                    munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
                    return false;
                }
                std::vector<uint32_t> lengths(static_cast<size_t>(col.value_count));
                if (length_bytes > 0) {
                    std::memcpy(lengths.data(), decoded_lengths.data(), length_bytes);
                }
                size_t total_bytes = 0;
                for (size_t j = 0; j < static_cast<size_t>(col.value_count); ++j) {
                    total_bytes += lengths[j];
                }
                std::vector<uint8_t> decoded_bytes(total_bytes);
                if (!DecodeLz4Literal(data_ptr, static_cast<size_t>(col.data_bytes),
                                      decoded_bytes.data(), decoded_bytes.size())) {
#if defined(__unix__) || defined(__APPLE__)
                    munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
                    return false;
                }
                    if (!field.LoadVarlen(lengths.data(), static_cast<size_t>(col.value_count),
                                          decoded_bytes.data(),
                                          static_cast<size_t>(decoded_bytes.size()))) {
#if defined(__unix__) || defined(__APPLE__)
                    munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
                    return false;
                }
                    offset += static_cast<size_t>(col.aux_bytes);
                } else {
                    const size_t raw_bytes = [&]() {
                    switch (field_type) {
                        case FieldType::kInt32:
                            return static_cast<size_t>(col.value_count) * sizeof(int32_t);
                        case FieldType::kInt64:
                            return static_cast<size_t>(col.value_count) * sizeof(int64_t);
                        case FieldType::kFloat64:
                            return static_cast<size_t>(col.value_count) * sizeof(double);
                        case FieldType::kBool:
                            return static_cast<size_t>(col.value_count) * sizeof(uint8_t);
                        case FieldType::kDictInt32:
                            return static_cast<size_t>(col.value_count) * sizeof(uint32_t);
                        default:
                            return static_cast<size_t>(0);
                    }
                }();
                std::vector<uint8_t> decoded(raw_bytes);
                if (!DecodeLz4Literal(data_ptr, static_cast<size_t>(col.data_bytes),
                                      decoded.data(), decoded.size())) {
#if defined(__unix__) || defined(__APPLE__)
                    munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
                    return false;
                }
                switch (field_type) {
                    case FieldType::kInt32:
                        std::memcpy(field.MutableInt32(), decoded.data(), decoded.size());
                        break;
                    case FieldType::kInt64:
                        std::memcpy(field.MutableInt64(), decoded.data(), decoded.size());
                        break;
                    case FieldType::kFloat64:
                        std::memcpy(field.MutableFloat64(), decoded.data(), decoded.size());
                        break;
                    case FieldType::kBool:
                        std::memcpy(field.MutableBool(), decoded.data(), decoded.size());
                        break;
                    case FieldType::kDictInt32:
                        std::memcpy(field.MutableDictIds(), decoded.data(), decoded.size());
                        break;
                    default:
                        return false;
                }
            }
        } else {
            CompressedColumnView view;
            view.kind = kind;
            view.type = field_type;
            view.row_count = static_cast<size_t>(col.value_count);
            view.ops = CompressionOpsFor(kind);
            std::vector<uint64_t> aligned_data(
                (static_cast<size_t>(col.data_bytes) + sizeof(uint64_t) - 1) /
                sizeof(uint64_t));
            std::vector<uint64_t> aligned_aux(
                (static_cast<size_t>(col.aux_bytes) + sizeof(uint64_t) - 1) /
                sizeof(uint64_t));
            if (col.data_bytes > 0) {
                std::memcpy(aligned_data.data(), data_ptr, static_cast<size_t>(col.data_bytes));
            }
            if (col.aux_bytes > 0) {
                std::memcpy(aligned_aux.data(), aux_ptr, static_cast<size_t>(col.aux_bytes));
            }
            view.data = aligned_data.empty()
                ? nullptr : reinterpret_cast<const uint8_t*>(aligned_data.data());
            view.data_size = static_cast<size_t>(col.data_bytes);
            view.aux = aligned_aux.empty()
                ? nullptr : reinterpret_cast<const uint8_t*>(aligned_aux.data());
            view.aux_size = static_cast<size_t>(col.aux_bytes);
            std::vector<uint64_t> aligned_validity;
            if (col.validity_words > 0) {
                const size_t validity_bytes =
                    static_cast<size_t>(col.validity_words) * sizeof(uint64_t);
                if (offset + col.aux_bytes + col.data_bytes + validity_bytes > mapped_size) {
#if defined(__unix__) || defined(__APPLE__)
                    munmap(const_cast<uint8_t*>(base), mapped_size);
#endif
                    return false;
                }
                aligned_validity.resize(static_cast<size_t>(col.validity_words));
                std::memcpy(aligned_validity.data(), data_ptr + col.data_bytes, validity_bytes);
                view.validity_words = aligned_validity.data();
                view.validity_word_count = static_cast<size_t>(col.validity_words);
                view.validity_bit_count = static_cast<size_t>(col.value_count);
            }
            switch (field_type) {
                case FieldType::kInt32: {
                    auto* out = field.MutableInt32();
                    for (size_t j = 0; j < view.row_count; ++j) {
                        int64_t value = 0;
                        if (ReadInt64Value(view, j, &value)) {
                            out[j] = static_cast<int32_t>(value);
                        } else {
                            out[j] = 0;
                        }
                    }
                    break;
                }
                case FieldType::kInt64: {
                    auto* out = field.MutableInt64();
                    for (size_t j = 0; j < view.row_count; ++j) {
                        int64_t value = 0;
                        if (ReadInt64Value(view, j, &value)) {
                            out[j] = value;
                        } else {
                            out[j] = 0;
                        }
                    }
                    break;
                }
                case FieldType::kBool: {
                    auto* out = field.MutableBool();
                    for (size_t j = 0; j < view.row_count; ++j) {
                        int64_t value = 0;
                        if (ReadInt64Value(view, j, &value)) {
                            out[j] = value != 0 ? 1 : 0;
                        } else {
                            out[j] = 0;
                        }
                    }
                    break;
                }
                case FieldType::kDictInt32: {
                    auto* out = field.MutableDictIds();
                    for (size_t j = 0; j < view.row_count; ++j) {
                        int64_t value = 0;
                        if (ReadInt64Value(view, j, &value)) {
                            out[j] = static_cast<uint32_t>(value);
                        } else {
                            out[j] = 0;
                        }
                    }
                    break;
                }
                default:
                    return false;
            }
            if (aux_bytes > 0) {
                offset += aux_bytes;
            }
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
            std::vector<uint64_t> validity(static_cast<size_t>(col.validity_words));
            std::memcpy(validity.data(), base + offset, validity_bytes);
            field.LoadValidityWords(validity.data(), validity.size(),
                                    static_cast<size_t>(col.value_count));
            offset += validity_bytes;
        }
        fields.push_back(std::move(field));
    }
    if (out_segment != nullptr) {
        Segment segment(static_cast<size_t>(header_.row_capacity),
                        static_cast<size_t>(header_.row_count),
                        std::move(fields),
                        false);
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

}  // namespace mimicdb
