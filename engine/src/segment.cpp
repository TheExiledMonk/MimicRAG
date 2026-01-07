#include "pcdb/segment.h"

#include "pcdb/hash.h"

namespace pcdb {

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

void Segment::ComputeStats() {
    stats_.clear();
    stats_.reserve(fields_.size());
    for (const auto& field : fields_) {
        SegmentColumnStats stats;
        const size_t count = field.Size();
        switch (field.Type()) {
            case FieldType::kInt32: {
                const auto* values = field.DataInt32();
                for (size_t i = 0; i < count; ++i) {
                    if (!field.IsValid(i)) {
                        ++stats.null_count;
                        continue;
                    }
                    const double value = static_cast<double>(values[i]);
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
        }
        stats_.push_back(stats);
    }
}

}  // namespace pcdb
