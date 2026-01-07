#include "pcdb/dataset.h"

#include "pcdb/hash.h"
#include "pcdb/config.h"

namespace pcdb {

Dataset::Dataset(std::string name)
    : name_(std::move(name)), segment_capacity_(Config::kDefaultSegmentRows) {}

const std::string& Dataset::Name() const {
    return name_;
}

size_t Dataset::RowCount() const {
    return rows_;
}

size_t Dataset::SegmentCapacity() const {
    return segment_capacity_;
}

void Dataset::AddField(FieldVector field) {
    fields_.push_back(std::move(field));
    active_fields_.push_back(fields_.back());
}

const std::vector<FieldVector>& Dataset::Fields() const {
    return fields_;
}

const std::vector<FieldVector>& Dataset::ActiveFields() const {
    return active_fields_;
}

size_t Dataset::ActiveRowCount() const {
    if (active_fields_.empty()) {
        return 0;
    }
    return active_fields_.front().Size();
}

const std::vector<Segment>& Dataset::Segments() const {
    return segments_;
}

FieldValue FieldValue::Int32(int32_t value) {
    FieldValue out;
    out.type = FieldType::kInt32;
    out.i32 = value;
    return out;
}

FieldValue FieldValue::Int64(int64_t value) {
    FieldValue out;
    out.type = FieldType::kInt64;
    out.i64 = value;
    return out;
}

FieldValue FieldValue::Float64(double value) {
    FieldValue out;
    out.type = FieldType::kFloat64;
    out.f64 = value;
    return out;
}

FieldValue FieldValue::Bool(bool value) {
    FieldValue out;
    out.type = FieldType::kBool;
    out.b = value;
    return out;
}

FieldValue FieldValue::Null(FieldType type) {
    FieldValue out;
    out.type = type;
    out.is_null = true;
    return out;
}

bool Dataset::Append(const std::vector<FieldValue>& values) {
    if (values.size() != fields_.size()) {
        return false;
    }
    const size_t active_rows = ActiveRowCount();
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (active_fields_[i].Size() != active_rows) {
            return false;
        }
        if (values[i].type != active_fields_[i].Type()) {
            return false;
        }
    }
    for (size_t i = 0; i < fields_.size(); ++i) {
        auto& field = active_fields_[i];
        const auto& value = values[i];
        bool ok = false;
        if (value.is_null) {
            ok = field.AppendNull();
        } else {
            switch (value.type) {
                case FieldType::kInt32:
                    ok = field.AppendInt32(value.i32);
                    break;
                case FieldType::kInt64:
                    ok = field.AppendInt64(value.i64);
                    break;
                case FieldType::kFloat64:
                    ok = field.AppendFloat64(value.f64);
                    break;
                case FieldType::kBool:
                    ok = field.AppendBool(value.b);
                    break;
                case FieldType::kDictInt32:
                    ok = field.AppendDictInt32(value.i32);
                    break;
            }
        }
        if (!ok) {
            return false;
        }
    }
    rows_ += 1;
    if (ActiveRowCount() >= segment_capacity_) {
        segments_.emplace_back(segment_capacity_, ActiveRowCount(), std::move(active_fields_));
        active_fields_.clear();
        for (const auto& field : fields_) {
            active_fields_.push_back(FieldVector(field.Name(), field.Type()));
        }
    }
    return true;
}

bool Dataset::AppendBatch(const std::vector<FieldBatch>& batches) {
    if (batches.size() != fields_.size()) {
        return false;
    }
    if (batches.empty()) {
        return true;
    }
    const size_t count = batches[0].count;
    if (count == 0) {
        return true;
    }
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (active_fields_[i].Size() != ActiveRowCount()) {
            return false;
        }
        if (batches[i].type != active_fields_[i].Type()) {
            return false;
        }
        if (batches[i].count != count) {
            return false;
        }
    }
    size_t offset = 0;
    while (offset < count) {
        const size_t active_rows = ActiveRowCount();
        const size_t capacity_left = segment_capacity_ - active_rows;
        const size_t take = (count - offset) < capacity_left ? (count - offset) : capacity_left;
        for (auto& field : active_fields_) {
            field.Reserve(active_rows + take);
        }
        for (size_t i = 0; i < fields_.size(); ++i) {
            auto& field = active_fields_[i];
            const auto& batch = batches[i];
            bool ok = false;
            switch (batch.type) {
                case FieldType::kInt32:
                    ok = field.AppendBatchInt32(
                        static_cast<const int32_t*>(batch.data) + offset, take,
                        batch.validity ? batch.validity + offset : nullptr);
                    break;
                case FieldType::kInt64:
                    ok = field.AppendBatchInt64(
                        static_cast<const int64_t*>(batch.data) + offset, take,
                        batch.validity ? batch.validity + offset : nullptr);
                    break;
                case FieldType::kFloat64:
                    ok = field.AppendBatchFloat64(
                        static_cast<const double*>(batch.data) + offset, take,
                        batch.validity ? batch.validity + offset : nullptr);
                    break;
                case FieldType::kBool:
                    ok = field.AppendBatchBool(
                        static_cast<const uint8_t*>(batch.data) + offset, take,
                        batch.validity ? batch.validity + offset : nullptr);
                    break;
                case FieldType::kDictInt32:
                    ok = field.AppendBatchDictInt32(
                        static_cast<const int32_t*>(batch.data) + offset, take,
                        batch.validity ? batch.validity + offset : nullptr);
                    break;
            }
            if (!ok) {
                return false;
            }
        }
        rows_ += take;
        offset += take;
        if (ActiveRowCount() >= segment_capacity_) {
            segments_.emplace_back(segment_capacity_, ActiveRowCount(), std::move(active_fields_));
            active_fields_.clear();
            for (const auto& field : fields_) {
                active_fields_.push_back(FieldVector(field.Name(), field.Type()));
            }
        }
    }
    return true;
}

uint64_t Dataset::SchemaFingerprint() const {
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

Schema Dataset::SchemaView() const {
    std::vector<SchemaField> fields;
    fields.reserve(fields_.size());
    for (const auto& field : fields_) {
        fields.push_back(SchemaField{field.Name(), field.Type()});
    }
    return Schema(std::move(fields));
}

}  // namespace pcdb
