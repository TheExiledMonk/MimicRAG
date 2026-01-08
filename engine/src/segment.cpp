#include "mimicdb/segment.h"

#include <cstdint>
#include <cstring>
#include <unordered_set>

#include "mimicdb/compression.h"
#include "mimicdb/config.h"
#include "mimicdb/hash.h"

namespace mimicdb {

namespace {
size_t ResolveRowCount(const std::vector<FieldVector>& fields, size_t explicit_count) {
    if (explicit_count != 0) {
        return explicit_count;
    }
    if (fields.empty()) {
        return 0;
    }
    return fields.front().Size();
}
}  // namespace

Segment::Segment(size_t row_capacity, std::vector<FieldVector> fields)
    : row_capacity_(row_capacity),
      fields_(std::move(fields)),
      format_{row_capacity} {
    row_count_ = ResolveRowCount(fields_, row_count_);
    if (row_count_ > row_capacity_) {
        row_count_ = row_capacity_;
    }
    sealed_ = row_count_ >= row_capacity_;
    ComputeStats();
}

Segment::Segment(size_t row_capacity, size_t row_count, std::vector<FieldVector> fields)
    : row_capacity_(row_capacity),
      row_count_(row_count),
      fields_(std::move(fields)),
      format_{row_capacity} {
    row_count_ = ResolveRowCount(fields_, row_count_);
    if (row_count_ > row_capacity_) {
        row_count_ = row_capacity_;
    }
    sealed_ = row_count_ >= row_capacity_;
    ComputeStats();
}

size_t Segment::RowCapacity() const {
    return row_capacity_;
}

size_t Segment::RowCount() const {
    return row_count_;
}

bool Segment::Append(size_t rows) {
    if (rows == 0 || sealed_ || row_count_ + rows > row_capacity_) {
        return false;
    }
    row_count_ += rows;
    if (row_count_ == row_capacity_) {
        sealed_ = true;
    }
    return true;
}

const SegmentFormat& Segment::Format() const {
    return format_;
}

uint64_t Segment::SchemaFingerprint() const {
    uint64_t hash = HashInit();
    const size_t field_count = fields_.size();
    hash = HashValue(hash, &field_count, sizeof(field_count));
    for (const auto& field : fields_) {
        const auto& name = field.Name();
        hash = HashString(hash, name);
        const auto type = static_cast<uint8_t>(field.Type());
        hash = HashValue(hash, &type, sizeof(type));
    }
    return hash;
}

const std::vector<FieldVector>& Segment::Fields() const {
    return fields_;
}

bool Segment::IsSealed() const {
    return sealed_;
}

const std::vector<SegmentColumnStats>& Segment::ColumnStats() const {
    return stats_;
}

const std::vector<ColumnCompressionKind>& Segment::CompressionKinds() const {
    return compression_kinds_;
}

void Segment::SetCompressionKinds(std::vector<ColumnCompressionKind> kinds) {
    if (kinds.size() != fields_.size()) {
        return;
    }
    compression_kinds_ = std::move(kinds);
    ResetCompression();
    BuildCompressionViews();
}

const std::vector<CompressedColumnView>& Segment::CompressedColumns() const {
    return compressed_columns_;
}

void Segment::ComputeStats() {
    stats_.clear();
    stats_.reserve(fields_.size());
    compression_kinds_.assign(fields_.size(), ColumnCompressionKind::kNone);
    for (size_t idx = 0; idx < fields_.size(); ++idx) {
        const auto& field = fields_[idx];
        SegmentColumnStats stats;
        const size_t count = field.Size();
        stats.value_count = count;
        bool non_decreasing = true;
        bool non_increasing = true;
        bool has_prev = false;
        double prev_value = 0.0;
        const size_t sample_target = 2048;
        const size_t stride = count > sample_target ? (count / sample_target) : 1;
        std::unordered_set<uint64_t> sample_hashes;
        if (count <= 4096) {
            sample_hashes.reserve(count);
        } else {
            sample_hashes.reserve(sample_target);
        }
        auto update_monotonic = [&](double value) {
            if (!has_prev) {
                prev_value = value;
                has_prev = true;
                return;
            }
            if (value < prev_value) {
                non_decreasing = false;
            }
            if (value > prev_value) {
                non_increasing = false;
            }
            prev_value = value;
        };
        auto add_sample = [&](uint64_t hash_value) {
            if (stride == 1 || sample_hashes.size() < sample_target) {
                sample_hashes.insert(hash_value);
            }
        };
        switch (field.Type()) {
            case FieldType::kInt32: {
                const auto* values = field.DataInt32();
                for (size_t i = 0; i < count; ++i) {
                    if (!field.IsValid(i)) {
                        ++stats.null_count;
                        continue;
                    }
                    const double value = static_cast<double>(values[i]);
                    update_monotonic(value);
                    if (i % stride == 0) {
                        uint64_t hash = HashInit();
                        hash = HashValue(hash, &values[i], sizeof(values[i]));
                        add_sample(hash);
                    }
                    if (!stats.has_value) {
                        stats.min = value;
                        stats.max = value;
                        stats.has_value = true;
                    } else {
                        if (value < stats.min) {
                            stats.min = value;
                        }
                        if (value > stats.max) {
                            stats.max = value;
                        }
                    }
                }
                break;
            }
            case FieldType::kInt64: {
                const auto* values = field.DataInt64();
                for (size_t i = 0; i < count; ++i) {
                    if (!field.IsValid(i)) {
                        ++stats.null_count;
                        continue;
                    }
                    const double value = static_cast<double>(values[i]);
                    update_monotonic(value);
                    if (i % stride == 0) {
                        uint64_t hash = HashInit();
                        hash = HashValue(hash, &values[i], sizeof(values[i]));
                        add_sample(hash);
                    }
                    if (!stats.has_value) {
                        stats.min = value;
                        stats.max = value;
                        stats.has_value = true;
                    } else {
                        if (value < stats.min) {
                            stats.min = value;
                        }
                        if (value > stats.max) {
                            stats.max = value;
                        }
                    }
                }
                break;
            }
            case FieldType::kFloat64: {
                const auto* values = field.DataFloat64();
                for (size_t i = 0; i < count; ++i) {
                    if (!field.IsValid(i)) {
                        ++stats.null_count;
                        continue;
                    }
                    const double value = values[i];
                    update_monotonic(value);
                    if (i % stride == 0) {
                        uint64_t bits = 0;
                        std::memcpy(&bits, &values[i], sizeof(bits));
                        uint64_t hash = HashInit();
                        hash = HashValue(hash, &bits, sizeof(bits));
                        add_sample(hash);
                    }
                    if (!stats.has_value) {
                        stats.min = value;
                        stats.max = value;
                        stats.has_value = true;
                    } else {
                        if (value < stats.min) {
                            stats.min = value;
                        }
                        if (value > stats.max) {
                            stats.max = value;
                        }
                    }
                }
                break;
            }
            case FieldType::kBool: {
                const auto* values = field.DataBool();
                for (size_t i = 0; i < count; ++i) {
                    if (!field.IsValid(i)) {
                        ++stats.null_count;
                        continue;
                    }
                    const double value = values[i] ? 1.0 : 0.0;
                    update_monotonic(value);
                    if (i % stride == 0) {
                        uint64_t hash = HashInit();
                        hash = HashValue(hash, &values[i], sizeof(values[i]));
                        add_sample(hash);
                    }
                    if (!stats.has_value) {
                        stats.min = value;
                        stats.max = value;
                        stats.has_value = true;
                    } else {
                        if (value < stats.min) {
                            stats.min = value;
                        }
                        if (value > stats.max) {
                            stats.max = value;
                        }
                    }
                }
                break;
            }
            case FieldType::kDictInt32: {
                const auto* values = field.DataDictIds();
                for (size_t i = 0; i < count; ++i) {
                    if (!field.IsValid(i)) {
                        ++stats.null_count;
                        continue;
                    }
                    const double value =
                        static_cast<double>(field.DictionaryValue(values[i]));
                    update_monotonic(value);
                    if (i % stride == 0) {
                        uint64_t hash = HashInit();
                        hash = HashValue(hash, &values[i], sizeof(values[i]));
                        add_sample(hash);
                    }
                    if (!stats.has_value) {
                        stats.min = value;
                        stats.max = value;
                        stats.has_value = true;
                    } else {
                        if (value < stats.min) {
                            stats.min = value;
                        }
                        if (value > stats.max) {
                            stats.max = value;
                        }
                    }
                }
                break;
            }
            case FieldType::kString:
            case FieldType::kBytes:
            case FieldType::kArray: {
                for (size_t i = 0; i < count; ++i) {
                    if (!field.IsValid(i)) {
                        ++stats.null_count;
                    }
                }
                stats.has_value = false;
                break;
            }
            case FieldType::kObject: {
                for (size_t i = 0; i < count; ++i) {
                    if (!field.IsValid(i)) {
                        ++stats.null_count;
                    }
                }
                stats.has_value = false;
                break;
            }
        }
        if (stats.has_value) {
            if (non_decreasing && !non_increasing) {
                stats.monotonic_hint = 1;
            } else if (non_increasing && !non_decreasing) {
                stats.monotonic_hint = 2;
            } else if (non_decreasing && non_increasing) {
                stats.monotonic_hint = 1;
            } else {
                stats.monotonic_hint = 3;
            }
        }
        if (!sample_hashes.empty()) {
            const size_t sample_count = (count + stride - 1) / stride;
            const double scale =
                sample_count > 0 ? static_cast<double>(count) / sample_count : 1.0;
            const double estimate = static_cast<double>(sample_hashes.size()) * scale;
            stats.estimated_cardinality =
                static_cast<uint32_t>(estimate > 0xFFFFFFFFu ? 0xFFFFFFFFu : estimate);
        }
        stats_.push_back(stats);
        compression_kinds_[idx] = ChooseCompression(stats, field.Type());
    }
    BuildCompressionViews();
}

void Segment::ResetCompression() {
    compressed_columns_.clear();
    compressed_data_.clear();
    compressed_aux_.clear();
    compressed_validity_.clear();
    compression_ready_ = false;
}

void Segment::BuildCompressionViews() {
    if (!sealed_ || compression_ready_) {
        return;
    }
    const size_t field_count = fields_.size();
    compressed_columns_.clear();
    compressed_data_.clear();
    compressed_aux_.clear();
    compressed_validity_.clear();
    compressed_columns_.reserve(field_count);
    compressed_data_.reserve(field_count);
    compressed_aux_.reserve(field_count);
    compressed_validity_.reserve(field_count);

    for (size_t i = 0; i < field_count; ++i) {
        auto& field = fields_[i];
        const auto type = field.Type();
        ColumnCompressionKind kind = compression_kinds_[i];
        const size_t raw_data_bytes = [&]() {
            switch (type) {
                case FieldType::kInt32:
                    return field.Size() * sizeof(int32_t);
                case FieldType::kInt64:
                    return field.Size() * sizeof(int64_t);
                case FieldType::kFloat64:
                    return field.Size() * sizeof(double);
                case FieldType::kBool:
                    return field.Size() * sizeof(uint8_t);
                case FieldType::kDictInt32:
                    return field.Size() * sizeof(uint32_t);
                case FieldType::kString:
                case FieldType::kBytes:
                case FieldType::kArray:
                    return field.BytesSize();
                case FieldType::kObject:
                    return static_cast<size_t>(0);
            }
            return static_cast<size_t>(0);
        }();
        const size_t raw_aux_bytes = (type == FieldType::kString ||
                                      type == FieldType::kBytes ||
                                      type == FieldType::kArray)
            ? field.Size() * sizeof(uint32_t)
            : 0;

        compression_kinds_[i] = kind;
        compressed_data_.emplace_back();
        compressed_aux_.emplace_back();
        compressed_validity_.emplace_back();
        auto& data = compressed_data_.back();
        auto& aux = compressed_aux_.back();
        auto& validity = compressed_validity_.back();
        switch (type) {
            case FieldType::kInt32: {
                const size_t bytes = field.Size() * sizeof(int32_t);
                if (kind == ColumnCompressionKind::kLz4) {
                    EncodeLz4Literal(reinterpret_cast<const uint8_t*>(field.DataInt32()),
                                     bytes, &data);
                } else {
                    data.resize(bytes);
                    std::memcpy(data.data(), field.DataInt32(), bytes);
                }
                break;
            }
            case FieldType::kInt64: {
                const size_t bytes = field.Size() * sizeof(int64_t);
                if (kind == ColumnCompressionKind::kLz4) {
                    EncodeLz4Literal(reinterpret_cast<const uint8_t*>(field.DataInt64()),
                                     bytes, &data);
                } else {
                    data.resize(bytes);
                    std::memcpy(data.data(), field.DataInt64(), bytes);
                }
                break;
            }
            case FieldType::kFloat64: {
                const size_t bytes = field.Size() * sizeof(double);
                if (kind == ColumnCompressionKind::kLz4) {
                    EncodeLz4Literal(reinterpret_cast<const uint8_t*>(field.DataFloat64()),
                                     bytes, &data);
                } else {
                    data.resize(bytes);
                    std::memcpy(data.data(), field.DataFloat64(), bytes);
                }
                break;
            }
            case FieldType::kBool: {
                const size_t bytes = field.Size() * sizeof(uint8_t);
                if (kind == ColumnCompressionKind::kLz4) {
                    EncodeLz4Literal(reinterpret_cast<const uint8_t*>(field.DataBool()),
                                     bytes, &data);
                } else {
                    data.resize(bytes);
                    std::memcpy(data.data(), field.DataBool(), bytes);
                }
                break;
            }
            case FieldType::kDictInt32: {
                const size_t bytes = field.Size() * sizeof(uint32_t);
                if (kind == ColumnCompressionKind::kLz4) {
                    EncodeLz4Literal(reinterpret_cast<const uint8_t*>(field.DataDictIds()),
                                     bytes, &data);
                } else {
                    data.resize(bytes);
                    std::memcpy(data.data(), field.DataDictIds(), bytes);
                }
                break;
            }
            case FieldType::kString:
            case FieldType::kBytes:
            case FieldType::kArray: {
                const size_t aux_bytes = field.Size() * sizeof(uint32_t);
                if (kind == ColumnCompressionKind::kLz4) {
                    EncodeLz4Literal(reinterpret_cast<const uint8_t*>(field.DataLengths()),
                                     aux_bytes, &aux);
                } else {
                    aux.resize(aux_bytes);
                    std::memcpy(aux.data(), field.DataLengths(), aux_bytes);
                }
                const size_t bytes = field.BytesSize();
                if (kind == ColumnCompressionKind::kLz4) {
                    EncodeLz4Literal(reinterpret_cast<const uint8_t*>(field.DataBytes()),
                                     bytes, &data);
                } else {
                    data.resize(bytes);
                    std::memcpy(data.data(), field.DataBytes(), bytes);
                }
                break;
            }
            case FieldType::kObject:
                break;
        }
        if (field.HasNulls()) {
            const size_t words = field.Validity().WordCount();
            validity.resize(words);
            std::memcpy(validity.data(), field.Validity().Words(), words * sizeof(uint64_t));
        }
        const size_t compressed_bytes =
            data.size() + aux.size() + validity.size() * sizeof(uint64_t);
        if (kind != ColumnCompressionKind::kNone) {
            const auto cfg = GetCompressionConfig();
            const double ratio = compressed_bytes == 0
                ? 0.0
                : static_cast<double>(raw_data_bytes + raw_aux_bytes) /
                    static_cast<double>(compressed_bytes);
            if (compressed_bytes > (raw_data_bytes + raw_aux_bytes) ||
                ratio < cfg.min_ratio) {
                kind = ColumnCompressionKind::kNone;
            }
        }

        compression_kinds_[i] = kind;
        if (kind == ColumnCompressionKind::kNone) {
            data.clear();
            aux.clear();
            validity.clear();
            compressed_columns_.push_back(MakeUncompressedView(field));
            continue;
        }

        CompressedColumnView view;
        view.kind = kind;
        view.type = type;
        view.row_count = field.Size();
        view.ops = kind == ColumnCompressionKind::kLz4 ? Lz4CompressionOps() : DefaultCompressionOps();
        view.data = data.empty() ? nullptr : data.data();
        view.data_size = data.size();
        view.aux = aux.empty() ? nullptr : aux.data();
        view.aux_size = aux.size();
        view.raw_data_size = raw_data_bytes;
        view.raw_aux_size = raw_aux_bytes;
        if (type == FieldType::kDictInt32) {
            view.dict = field.Dictionary();
        }
        view.validity_words = validity.empty() ? nullptr : validity.data();
        view.validity_word_count = validity.size();
        view.validity_bit_count = field.Validity().Size();
        compressed_columns_.push_back(view);
        field.ReleaseStorage();
    }
    compression_ready_ = true;
}

}  // namespace mimicdb
