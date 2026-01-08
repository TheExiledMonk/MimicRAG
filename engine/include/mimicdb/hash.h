#ifndef MIMICDB_HASH_H
#define MIMICDB_HASH_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace mimicdb {

uint64_t HashBytes(uint64_t hash, const void* data, size_t length);
uint64_t HashString(uint64_t hash, const std::string& value);
uint64_t HashValue(uint64_t hash, const void* data, size_t length);

uint64_t HashInit();

}  // namespace mimicdb

#endif  // MIMICDB_HASH_H
