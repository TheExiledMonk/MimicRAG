#ifndef MIMICDB_SCHEMA_H
#define MIMICDB_SCHEMA_H

#include <cstdint>
#include <string>
#include <vector>

#include "mimicdb/types.h"

namespace mimicdb {

struct SchemaField {
    std::string name;
    FieldType type;
};

class Schema {
public:
    Schema() = default;
    explicit Schema(std::vector<SchemaField> fields);

    const std::vector<SchemaField>& Fields() const;
    uint64_t Fingerprint() const;

private:
    std::vector<SchemaField> fields_;
};

}  // namespace mimicdb

#endif  // MIMICDB_SCHEMA_H
