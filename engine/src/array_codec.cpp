#include "mimicdb/array_codec.h"

#include <cstring>

namespace mimicdb {
namespace {

void AppendBytes(std::string* out, const void* data, size_t size) {
    out->append(reinterpret_cast<const char*>(data), size);
}

template <typename T>
void AppendValue(std::string* out, const T& value) {
    AppendBytes(out, &value, sizeof(T));
}

bool ReadBytes(const std::string& bytes, size_t* offset, void* out, size_t size) {
    if (*offset + size > bytes.size()) {
        return false;
    }
    std::memcpy(out, bytes.data() + *offset, size);
    *offset += size;
    return true;
}

}  // namespace

std::string EncodeArray(const std::vector<FieldValue>& values) {
    std::string out;
    const uint32_t count = static_cast<uint32_t>(values.size());
    AppendValue(&out, count);
    for (const auto& value : values) {
        const uint8_t type = static_cast<uint8_t>(value.type);
        const uint8_t is_null = value.is_null ? 1U : 0U;
        const uint16_t reserved = 0;
        AppendValue(&out, type);
        AppendValue(&out, is_null);
        AppendValue(&out, reserved);
        std::string payload;
        if (!value.is_null) {
            switch (value.type) {
                case FieldType::kInt32:
                    AppendValue(&payload, value.i32);
                    break;
                case FieldType::kInt64:
                    AppendValue(&payload, value.i64);
                    break;
                case FieldType::kFloat64:
                    AppendValue(&payload, value.f64);
                    break;
                case FieldType::kBool: {
                    const uint8_t b = value.b ? 1U : 0U;
                    AppendValue(&payload, b);
                    break;
                }
                case FieldType::kDictInt32:
                    AppendValue(&payload, value.i32);
                    break;
                case FieldType::kString:
                case FieldType::kBytes:
                    payload = value.bytes;
                    break;
                case FieldType::kArray:
                    payload = EncodeArray(value.array);
                    break;
                case FieldType::kObject:
                    payload.clear();
                    break;
            }
        }
        const uint32_t size = static_cast<uint32_t>(payload.size());
        AppendValue(&out, size);
        if (size > 0) {
            AppendBytes(&out, payload.data(), payload.size());
        }
    }
    return out;
}

bool DecodeArray(const std::string& bytes, std::vector<FieldValue>* out) {
    if (!out) {
        return false;
    }
    out->clear();
    size_t offset = 0;
    uint32_t count = 0;
    if (!ReadBytes(bytes, &offset, &count, sizeof(count))) {
        return false;
    }
    out->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t type = 0;
        uint8_t is_null = 0;
        uint16_t reserved = 0;
        uint32_t size = 0;
        if (!ReadBytes(bytes, &offset, &type, sizeof(type)) ||
            !ReadBytes(bytes, &offset, &is_null, sizeof(is_null)) ||
            !ReadBytes(bytes, &offset, &reserved, sizeof(reserved)) ||
            !ReadBytes(bytes, &offset, &size, sizeof(size))) {
            return false;
        }
        if (offset + size > bytes.size()) {
            return false;
        }
        FieldValue value;
        value.type = static_cast<FieldType>(type);
        value.is_null = is_null != 0;
        if (!value.is_null) {
            switch (value.type) {
                case FieldType::kInt32: {
                    if (size != sizeof(int32_t)) {
                        return false;
                    }
                    std::memcpy(&value.i32, bytes.data() + offset, sizeof(int32_t));
                    break;
                }
                case FieldType::kInt64: {
                    if (size != sizeof(int64_t)) {
                        return false;
                    }
                    std::memcpy(&value.i64, bytes.data() + offset, sizeof(int64_t));
                    break;
                }
                case FieldType::kFloat64: {
                    if (size != sizeof(double)) {
                        return false;
                    }
                    std::memcpy(&value.f64, bytes.data() + offset, sizeof(double));
                    break;
                }
                case FieldType::kBool: {
                    if (size != sizeof(uint8_t)) {
                        return false;
                    }
                    value.b = bytes[offset] != 0;
                    break;
                }
                case FieldType::kDictInt32: {
                    if (size != sizeof(int32_t)) {
                        return false;
                    }
                    std::memcpy(&value.i32, bytes.data() + offset, sizeof(int32_t));
                    break;
                }
                case FieldType::kString:
                case FieldType::kBytes:
                    value.bytes.assign(bytes.data() + offset, size);
                    break;
                case FieldType::kArray: {
                    std::string nested(bytes.data() + offset, size);
                    if (!DecodeArray(nested, &value.array)) {
                        return false;
                    }
                    break;
                }
                case FieldType::kObject:
                    value.object.clear();
                    break;
            }
        }
        offset += size;
        out->push_back(std::move(value));
    }
    return true;
}

}  // namespace mimicdb
