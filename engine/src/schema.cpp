#include "pcdb/schema.h"

#include "pcdb/hash.h"

namespace pcdb {

Schema::Schema(std::vector<SchemaField> fields) : fields_(std::move(fields)) {}

const std::vector<SchemaField>& Schema::Fields() const {
    return fields_;
}

uint64_t Schema::Fingerprint() const {
    uint64_t hash = HashInit();
    const size_t field_count = fields_.size();
    hash = HashValue(hash, &field_count, sizeof(field_count));
    for (const auto& field : fields_) {
        hash = HashString(hash, field.name);
        const auto type = static_cast<uint8_t>(field.type);
        hash = HashValue(hash, &type, sizeof(type));
    }
    return hash;
}

}  // namespace pcdb
