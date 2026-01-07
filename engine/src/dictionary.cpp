#include "pcdb/dictionary.h"

namespace pcdb {

uint32_t DictionaryInt32::Add(int32_t value) {
    auto it = index_.find(value);
    if (it != index_.end()) {
        return it->second;
    }
    const uint32_t id = static_cast<uint32_t>(values_.size());
    values_.push_back(value);
    index_[value] = id;
    return id;
}

int32_t DictionaryInt32::Value(uint32_t id) const {
    return values_[id];
}

uint32_t DictionaryInt32::Size() const {
    return static_cast<uint32_t>(values_.size());
}

}  // namespace pcdb
