#ifndef MIMICDB_TYPES_H
#define MIMICDB_TYPES_H

#include <cstdint>

namespace mimicdb {

enum class FieldType : uint8_t {
    kInt32,
    kInt64,
    kFloat64,
    kBool,
    kDictInt32,
    kString,
    kBytes,
    kArray,
    kObject,
    kVectorFloat32,
};

}  // namespace mimicdb

#endif  // MIMICDB_TYPES_H
