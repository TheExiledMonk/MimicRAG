#ifndef MIMICDB_FIELD_VECTOR_H
#define MIMICDB_FIELD_VECTOR_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mimicdb/dictionary.h"
#include "mimicdb/bitmap.h"
#include "mimicdb/types.h"

namespace mimicdb {

class FieldVector {
public:
    FieldVector(std::string name, FieldType type);

    const std::string& Name() const;
    FieldType Type() const;
    size_t Size() const;
    bool HasNulls() const;

    void Resize(size_t size);
    bool AppendInt32(int32_t value);
    bool AppendInt64(int64_t value);
    bool AppendFloat64(double value);
    bool AppendBool(bool value);
    bool AppendDictInt32(int32_t value);
    bool AppendString(const std::string& value);
    bool AppendBytes(const std::string& value);
    bool AppendVectorFloat32(const float* values, size_t dimension);
    bool AppendNull();
    bool AppendBatchInt32(const int32_t* values, size_t count, const uint8_t* validity);
    bool AppendBatchInt64(const int64_t* values, size_t count, const uint8_t* validity);
    bool AppendBatchFloat64(const double* values, size_t count, const uint8_t* validity);
    bool AppendBatchBool(const uint8_t* values, size_t count, const uint8_t* validity);
    bool AppendBatchDictInt32(const int32_t* values, size_t count, const uint8_t* validity);
    bool AppendBatchString(const uint32_t* lengths, const uint8_t* bytes, size_t count,
                           const uint8_t* validity);
    bool AppendBatchBytes(const uint32_t* lengths, const uint8_t* bytes, size_t count,
                          const uint8_t* validity);
    void SetValid(size_t index, bool valid);
    bool IsValid(size_t index) const;
    const Bitmap& Validity() const;

    const int32_t* DataInt32() const;
    const int64_t* DataInt64() const;
    const double* DataFloat64() const;
    const uint8_t* DataBool() const;
    const uint32_t* DataDictIds() const;
    const DictionaryInt32* Dictionary() const;
    uint32_t DictionarySize() const;
    int32_t DictionaryValue(uint32_t id) const;
    const uint8_t* DataBytes() const;
    const uint32_t* DataLengths() const;
    size_t BytesSize() const;
    const float* VectorFloat32(size_t index, size_t* dimension) const;
    const uint32_t* DataOffsets() const;

    int32_t* MutableInt32();
    int64_t* MutableInt64();
    double* MutableFloat64();
    uint8_t* MutableBool();
    uint32_t* MutableDictIds();

    bool LoadValidityWords(const uint64_t* words, size_t word_count, size_t bit_count);
    bool LoadVarlen(const uint32_t* lengths, size_t count, const uint8_t* bytes, size_t bytes_size);
    void Reserve(size_t size);
    void ReleaseStorage();

private:
    std::string name_;
    FieldType type_;
    size_t size_ = 0;
    Bitmap validity_;
    std::vector<int32_t> data_i32_;
    std::vector<int64_t> data_i64_;
    std::vector<double> data_f64_;
    std::vector<uint8_t> data_bool_;
    std::vector<uint32_t> data_dict_ids_;
    DictionaryInt32 dictionary_;
    std::vector<uint8_t> data_bytes_;
    std::vector<uint32_t> data_lengths_;
    mutable std::vector<uint32_t> data_offsets_;
    mutable bool offsets_valid_ = false;

    bool ResizeStorage(size_t size);
    void AppendValidity(const uint8_t* validity, size_t count);
};

}  // namespace mimicdb

#endif  // MIMICDB_FIELD_VECTOR_H
