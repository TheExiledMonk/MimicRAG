#include "mimicdb/field_vector.h"

#include <cstring>

namespace mimicdb {

FieldVector::FieldVector(std::string name, FieldType type)
    : name_(std::move(name)), type_(type) {}

const std::string& FieldVector::Name() const {
    return name_;
}

FieldType FieldVector::Type() const {
    return type_;
}

size_t FieldVector::Size() const {
    return size_;
}

bool FieldVector::HasNulls() const {
    return validity_.Size() != 0;
}

void FieldVector::Resize(size_t size) {
    if (size <= size_) {
        return;
    }
    if (!ResizeStorage(size)) {
        return;
    }
    if (validity_.Size() != 0) {
        validity_.Resize(size);
        for (size_t i = size_; i < size; ++i) {
            validity_.Set(i, true);
        }
    }
    size_ = size;
}

void FieldVector::SetValid(size_t index, bool valid) {
    if (validity_.Size() == 0) {
        validity_.Resize(size_);
        for (size_t i = 0; i < size_; ++i) {
            validity_.Set(i, true);
        }
    }
    validity_.Set(index, valid);
}

bool FieldVector::IsValid(size_t index) const {
    if (validity_.Size() == 0) {
        return true;
    }
    return validity_.Get(index);
}

const Bitmap& FieldVector::Validity() const {
    return validity_;
}

const int32_t* FieldVector::DataInt32() const {
    return type_ == FieldType::kInt32 ? data_i32_.data() : nullptr;
}

const int64_t* FieldVector::DataInt64() const {
    return type_ == FieldType::kInt64 ? data_i64_.data() : nullptr;
}

const double* FieldVector::DataFloat64() const {
    return type_ == FieldType::kFloat64 ? data_f64_.data() : nullptr;
}

const uint8_t* FieldVector::DataBool() const {
    return type_ == FieldType::kBool ? data_bool_.data() : nullptr;
}

const uint32_t* FieldVector::DataDictIds() const {
    return type_ == FieldType::kDictInt32 ? data_dict_ids_.data() : nullptr;
}

const DictionaryInt32* FieldVector::Dictionary() const {
    return type_ == FieldType::kDictInt32 ? &dictionary_ : nullptr;
}

uint32_t FieldVector::DictionarySize() const {
    return type_ == FieldType::kDictInt32 ? dictionary_.Size() : 0;
}

int32_t FieldVector::DictionaryValue(uint32_t id) const {
    return dictionary_.Value(id);
}

const uint8_t* FieldVector::DataBytes() const {
    return (type_ == FieldType::kString || type_ == FieldType::kBytes ||
            type_ == FieldType::kArray || type_ == FieldType::kVectorFloat32)
               ? data_bytes_.data()
               : nullptr;
}

const uint32_t* FieldVector::DataLengths() const {
    return (type_ == FieldType::kString || type_ == FieldType::kBytes ||
            type_ == FieldType::kArray || type_ == FieldType::kVectorFloat32)
               ? data_lengths_.data()
               : nullptr;
}

const uint32_t* FieldVector::DataOffsets() const {
    if (type_ != FieldType::kString && type_ != FieldType::kBytes &&
        type_ != FieldType::kArray && type_ != FieldType::kVectorFloat32) {
        return nullptr;
    }
    if (offsets_valid_) {
        return data_offsets_.empty() ? nullptr : data_offsets_.data();
    }
    data_offsets_.resize(data_lengths_.size() + 1);
    uint32_t offset = 0;
    data_offsets_[0] = 0;
    for (size_t i = 0; i < data_lengths_.size(); ++i) {
        offset += data_lengths_[i];
        data_offsets_[i + 1] = offset;
    }
    offsets_valid_ = true;
    return data_offsets_.empty() ? nullptr : data_offsets_.data();
}

size_t FieldVector::BytesSize() const {
    return (type_ == FieldType::kString || type_ == FieldType::kBytes ||
            type_ == FieldType::kArray || type_ == FieldType::kVectorFloat32)
               ? data_bytes_.size()
               : 0;
}

int32_t* FieldVector::MutableInt32() {
    return type_ == FieldType::kInt32 ? data_i32_.data() : nullptr;
}

int64_t* FieldVector::MutableInt64() {
    return type_ == FieldType::kInt64 ? data_i64_.data() : nullptr;
}

double* FieldVector::MutableFloat64() {
    return type_ == FieldType::kFloat64 ? data_f64_.data() : nullptr;
}

uint8_t* FieldVector::MutableBool() {
    return type_ == FieldType::kBool ? data_bool_.data() : nullptr;
}

uint32_t* FieldVector::MutableDictIds() {
    return type_ == FieldType::kDictInt32 ? data_dict_ids_.data() : nullptr;
}

bool FieldVector::LoadValidityWords(const uint64_t* words, size_t word_count,
                                    size_t bit_count) {
    validity_.LoadWords(words, word_count, bit_count);
    return true;
}

bool FieldVector::LoadVarlen(const uint32_t* lengths, size_t count, const uint8_t* bytes,
                             size_t bytes_size) {
    if (type_ != FieldType::kString && type_ != FieldType::kBytes &&
        type_ != FieldType::kArray && type_ != FieldType::kVectorFloat32) {
        return false;
    }
    data_lengths_.resize(count, 0);
    if (count > 0) {
        std::memcpy(data_lengths_.data(), lengths, count * sizeof(uint32_t));
    }
    data_bytes_.resize(bytes_size);
    if (bytes_size > 0) {
        std::memcpy(data_bytes_.data(), bytes, bytes_size);
    }
    offsets_valid_ = false;
    size_ = count;
    return true;
}

bool FieldVector::AppendInt32(int32_t value) {
    if (type_ != FieldType::kInt32) {
        return false;
    }
    data_i32_.push_back(value);
    if (validity_.Size() != 0) {
        validity_.Resize(data_i32_.size());
        validity_.Set(data_i32_.size() - 1, true);
    }
    size_ = data_i32_.size();
    return true;
}

bool FieldVector::AppendInt64(int64_t value) {
    if (type_ != FieldType::kInt64) {
        return false;
    }
    data_i64_.push_back(value);
    if (validity_.Size() != 0) {
        validity_.Resize(data_i64_.size());
        validity_.Set(data_i64_.size() - 1, true);
    }
    size_ = data_i64_.size();
    return true;
}

bool FieldVector::AppendFloat64(double value) {
    if (type_ != FieldType::kFloat64) {
        return false;
    }
    data_f64_.push_back(value);
    if (validity_.Size() != 0) {
        validity_.Resize(data_f64_.size());
        validity_.Set(data_f64_.size() - 1, true);
    }
    size_ = data_f64_.size();
    return true;
}

bool FieldVector::AppendBool(bool value) {
    if (type_ != FieldType::kBool) {
        return false;
    }
    data_bool_.push_back(value ? 1U : 0U);
    if (validity_.Size() != 0) {
        validity_.Resize(data_bool_.size());
        validity_.Set(data_bool_.size() - 1, true);
    }
    size_ = data_bool_.size();
    return true;
}

bool FieldVector::AppendDictInt32(int32_t value) {
    if (type_ != FieldType::kDictInt32) {
        return false;
    }
    const uint32_t id = dictionary_.Add(value);
    data_dict_ids_.push_back(id);
    if (validity_.Size() != 0) {
        validity_.Resize(data_dict_ids_.size());
        validity_.Set(data_dict_ids_.size() - 1, true);
    }
    size_ = data_dict_ids_.size();
    return true;
}

bool FieldVector::AppendString(const std::string& value) {
    if (type_ != FieldType::kString) {
        return false;
    }
    data_lengths_.push_back(static_cast<uint32_t>(value.size()));
    data_bytes_.insert(data_bytes_.end(), value.begin(), value.end());
    offsets_valid_ = false;
    if (validity_.Size() != 0) {
        validity_.Resize(data_lengths_.size());
        validity_.Set(data_lengths_.size() - 1, true);
    }
    size_ = data_lengths_.size();
    return true;
}

bool FieldVector::AppendBytes(const std::string& value) {
    if (type_ != FieldType::kBytes && type_ != FieldType::kArray &&
        type_ != FieldType::kVectorFloat32) {
        return false;
    }
    data_lengths_.push_back(static_cast<uint32_t>(value.size()));
    data_bytes_.insert(data_bytes_.end(),
                       reinterpret_cast<const uint8_t*>(value.data()),
                       reinterpret_cast<const uint8_t*>(value.data()) + value.size());
    offsets_valid_ = false;
    if (validity_.Size() != 0) {
        validity_.Resize(data_lengths_.size());
        validity_.Set(data_lengths_.size() - 1, true);
    }
    size_ = data_lengths_.size();
    return true;
}

bool FieldVector::AppendVectorFloat32(const float* values, size_t dimension) {
    if (type_ != FieldType::kVectorFloat32 || (!values && dimension != 0) ||
        dimension > UINT32_MAX / sizeof(float)) return false;
    return AppendBytes(std::string(reinterpret_cast<const char*>(values),
                                   dimension * sizeof(float)));
}

const float* FieldVector::VectorFloat32(size_t index, size_t* dimension) const {
    if (dimension) *dimension = 0;
    if (type_ != FieldType::kVectorFloat32 || index >= size_ || !IsValid(index)) return nullptr;
    const auto* offsets = DataOffsets();
    if (!offsets || data_lengths_[index] % sizeof(float) != 0) return nullptr;
    if (dimension) *dimension = data_lengths_[index] / sizeof(float);
    return reinterpret_cast<const float*>(data_bytes_.data() + offsets[index]);
}

bool FieldVector::AppendNull() {
    if (!ResizeStorage(size_ + 1)) {
        return false;
    }
    if (validity_.Size() == 0) {
        validity_.Resize(size_);
        for (size_t i = 0; i < size_; ++i) {
            validity_.Set(i, true);
        }
    }
    validity_.Resize(size_ + 1);
    validity_.Set(size_, false);
    if (type_ == FieldType::kString || type_ == FieldType::kBytes ||
        type_ == FieldType::kArray || type_ == FieldType::kVectorFloat32) {
        if (data_lengths_.size() == size_ + 1) {
            data_lengths_[size_] = 0;
        } else {
            data_lengths_.push_back(0);
        }
        offsets_valid_ = false;
    }
    size_ += 1;
    return true;
}

void FieldVector::Reserve(size_t size) {
    switch (type_) {
        case FieldType::kInt32:
            data_i32_.reserve(size);
            break;
        case FieldType::kInt64:
            data_i64_.reserve(size);
            break;
        case FieldType::kFloat64:
            data_f64_.reserve(size);
            break;
        case FieldType::kBool:
            data_bool_.reserve(size);
            break;
        case FieldType::kDictInt32:
            data_dict_ids_.reserve(size);
            break;
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
        case FieldType::kVectorFloat32:
            data_lengths_.reserve(size);
            break;
        case FieldType::kObject:
            break;
    }
}

void FieldVector::ReleaseStorage() {
    data_i32_.clear();
    data_i64_.clear();
    data_f64_.clear();
    data_bool_.clear();
    data_dict_ids_.clear();
    data_bytes_.clear();
    data_lengths_.clear();
    data_offsets_.clear();
    offsets_valid_ = false;
    std::vector<int32_t>().swap(data_i32_);
    std::vector<int64_t>().swap(data_i64_);
    std::vector<double>().swap(data_f64_);
    std::vector<uint8_t>().swap(data_bool_);
    std::vector<uint32_t>().swap(data_dict_ids_);
    std::vector<uint8_t>().swap(data_bytes_);
    std::vector<uint32_t>().swap(data_lengths_);
    std::vector<uint32_t>().swap(data_offsets_);
}

void FieldVector::ReleaseValuesStorage() {
    if (type_ == FieldType::kVectorFloat32 || type_ == FieldType::kString ||
        type_ == FieldType::kBytes || type_ == FieldType::kArray) {
        std::vector<uint8_t>().swap(data_bytes_);
        std::vector<uint32_t>().swap(data_offsets_);
        offsets_valid_ = false;
    }
}

bool FieldVector::ValuesResident() const {
    if (type_ != FieldType::kVectorFloat32 && type_ != FieldType::kString &&
        type_ != FieldType::kBytes && type_ != FieldType::kArray) return true;
    size_t expected = 0; for (uint32_t length : data_lengths_) expected += length;
    return expected == data_bytes_.size();
}

void FieldVector::AppendValidity(const uint8_t* validity, size_t count) {
    if (validity_.Size() == 0) {
        validity_.Resize(size_);
        for (size_t i = 0; i < size_; ++i) {
            validity_.Set(i, true);
        }
    }
    validity_.AppendBits(validity, count);
}

bool FieldVector::AppendBatchInt32(const int32_t* values, size_t count, const uint8_t* validity) {
    if (type_ != FieldType::kInt32) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    const size_t start = data_i32_.size();
    data_i32_.resize(start + count, 0);
    std::memcpy(data_i32_.data() + start, values, count * sizeof(int32_t));
    if (validity) {
        AppendValidity(validity, count);
    } else if (validity_.Size() != 0) {
        validity_.AppendAllTrue(count);
    }
    size_ = data_i32_.size();
    return true;
}

bool FieldVector::AppendBatchInt64(const int64_t* values, size_t count, const uint8_t* validity) {
    if (type_ != FieldType::kInt64) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    const size_t start = data_i64_.size();
    data_i64_.resize(start + count, 0);
    std::memcpy(data_i64_.data() + start, values, count * sizeof(int64_t));
    if (validity) {
        AppendValidity(validity, count);
    } else if (validity_.Size() != 0) {
        validity_.AppendAllTrue(count);
    }
    size_ = data_i64_.size();
    return true;
}

bool FieldVector::AppendBatchFloat64(const double* values, size_t count, const uint8_t* validity) {
    if (type_ != FieldType::kFloat64) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    const size_t start = data_f64_.size();
    data_f64_.resize(start + count, 0.0);
    std::memcpy(data_f64_.data() + start, values, count * sizeof(double));
    if (validity) {
        AppendValidity(validity, count);
    } else if (validity_.Size() != 0) {
        validity_.AppendAllTrue(count);
    }
    size_ = data_f64_.size();
    return true;
}

bool FieldVector::AppendBatchBool(const uint8_t* values, size_t count, const uint8_t* validity) {
    if (type_ != FieldType::kBool) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    const size_t start = data_bool_.size();
    data_bool_.resize(start + count, 0);
    std::memcpy(data_bool_.data() + start, values, count * sizeof(uint8_t));
    if (validity) {
        AppendValidity(validity, count);
    } else if (validity_.Size() != 0) {
        validity_.AppendAllTrue(count);
    }
    size_ = data_bool_.size();
    return true;
}

bool FieldVector::AppendBatchDictInt32(const int32_t* values, size_t count,
                                       const uint8_t* validity) {
    if (type_ != FieldType::kDictInt32) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    const size_t start = data_dict_ids_.size();
    data_dict_ids_.resize(start + count, 0);
    for (size_t i = 0; i < count; ++i) {
        data_dict_ids_[start + i] = dictionary_.Add(values[i]);
    }
    if (validity) {
        AppendValidity(validity, count);
    } else if (validity_.Size() != 0) {
        validity_.AppendAllTrue(count);
    }
    size_ = data_dict_ids_.size();
    return true;
}

bool FieldVector::AppendBatchString(const uint32_t* lengths, const uint8_t* bytes,
                                    size_t count, const uint8_t* validity) {
    if (type_ != FieldType::kString) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    const size_t start = data_lengths_.size();
    data_lengths_.resize(start + count, 0);
    std::memcpy(data_lengths_.data() + start, lengths, count * sizeof(uint32_t));
    offsets_valid_ = false;
    size_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t len = lengths[i];
        if (len == 0) {
            continue;
        }
        data_bytes_.insert(data_bytes_.end(), bytes + offset, bytes + offset + len);
        offset += len;
    }
    if (validity) {
        AppendValidity(validity, count);
    } else if (validity_.Size() != 0) {
        validity_.AppendAllTrue(count);
    }
    size_ = data_lengths_.size();
    return true;
}

bool FieldVector::AppendBatchBytes(const uint32_t* lengths, const uint8_t* bytes,
                                   size_t count, const uint8_t* validity) {
    if (type_ != FieldType::kBytes && type_ != FieldType::kArray &&
        type_ != FieldType::kVectorFloat32) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    const size_t start = data_lengths_.size();
    data_lengths_.resize(start + count, 0);
    std::memcpy(data_lengths_.data() + start, lengths, count * sizeof(uint32_t));
    offsets_valid_ = false;
    size_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t len = lengths[i];
        if (len == 0) {
            continue;
        }
        data_bytes_.insert(data_bytes_.end(), bytes + offset, bytes + offset + len);
        offset += len;
    }
    if (validity) {
        AppendValidity(validity, count);
    } else if (validity_.Size() != 0) {
        validity_.AppendAllTrue(count);
    }
    size_ = data_lengths_.size();
    return true;
}

bool FieldVector::ResizeStorage(size_t size) {
    switch (type_) {
        case FieldType::kInt32:
            data_i32_.resize(size, 0);
            break;
        case FieldType::kInt64:
            data_i64_.resize(size, 0);
            break;
        case FieldType::kFloat64:
            data_f64_.resize(size, 0.0);
            break;
        case FieldType::kBool:
            data_bool_.resize(size, 0);
            break;
        case FieldType::kDictInt32:
            data_dict_ids_.resize(size, 0);
            break;
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
        case FieldType::kVectorFloat32:
            data_lengths_.resize(size, 0);
            break;
        case FieldType::kObject:
            break;
    }
    return true;
}

}  // namespace mimicdb
