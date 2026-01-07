#ifndef PCDB_TYPES_H
#define PCDB_TYPES_H

#include <cstdint>

namespace pcdb {

enum class FieldType : uint8_t {
    kInt32,
    kInt64,
    kFloat64,
    kBool,
    kDictInt32,
};

}  // namespace pcdb

#endif  // PCDB_TYPES_H
