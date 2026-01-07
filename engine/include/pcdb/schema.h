#ifndef PCDB_SCHEMA_H
#define PCDB_SCHEMA_H

#include <cstdint>
#include <string>
#include <vector>

#include "pcdb/types.h"

namespace pcdb {

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

}  // namespace pcdb

#endif  // PCDB_SCHEMA_H
