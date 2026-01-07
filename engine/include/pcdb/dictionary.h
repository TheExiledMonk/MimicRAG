#ifndef PCDB_DICTIONARY_H
#define PCDB_DICTIONARY_H

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace pcdb {

class DictionaryInt32 {
public:
    uint32_t Add(int32_t value);
    int32_t Value(uint32_t id) const;
    uint32_t Size() const;

private:
    std::unordered_map<int32_t, uint32_t> index_;
    std::vector<int32_t> values_;
};

}  // namespace pcdb

#endif  // PCDB_DICTIONARY_H
