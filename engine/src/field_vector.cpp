#include "pcdb/field_vector.h"

#include <cstring>

namespace pcdb {

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
    }
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
    }
    return true;
}

}  // namespace pcdb
