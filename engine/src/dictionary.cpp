#include "mimicdb/dictionary.h"

namespace mimicdb {

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

bool DictionaryInt32::FindId(int32_t value, uint32_t* out_id) const {
    if (!out_id) {
        return false;
    }
    const auto it = index_.find(value);
    if (it == index_.end()) {
        return false;
    }
    *out_id = it->second;
    return true;
}

}  // namespace mimicdb
