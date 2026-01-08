#ifndef MIMICDB_DICTIONARY_H
#define MIMICDB_DICTIONARY_H

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mimicdb {

class DictionaryInt32 {
public:
    uint32_t Add(int32_t value);
    int32_t Value(uint32_t id) const;
    uint32_t Size() const;
    bool FindId(int32_t value, uint32_t* out_id) const;

private:
    std::unordered_map<int32_t, uint32_t> index_;
    std::vector<int32_t> values_;
};

}  // namespace mimicdb

#endif  // MIMICDB_DICTIONARY_H
