#include "pcdb/projection.h"

#include <cstddef>

#include "pcdb/field_vector.h"

namespace pcdb {

ProjectionResult ProjectRows(const std::vector<FieldVector>& fields,
                             const Projection& projection,
                             const std::vector<size_t>& row_ids) {
    ProjectionResult result;
    result.fields.reserve(projection.indices.size());
    for (size_t index : projection.indices) {
        FieldVector out(fields[index].Name(), fields[index].Type());
        result.fields.push_back(std::move(out));
    }

    for (size_t row : row_ids) {
        for (size_t i = 0; i < projection.indices.size(); ++i) {
            const auto& field = fields[projection.indices[i]];
            auto& dest = result.fields[i];
            if (!field.IsValid(row)) {
                dest.AppendNull();
                continue;
            }
            switch (field.Type()) {
                case FieldType::kInt32:
                    dest.AppendInt32(field.DataInt32()[row]);
                    break;
                case FieldType::kInt64:
                    dest.AppendInt64(field.DataInt64()[row]);
                    break;
                case FieldType::kFloat64:
                    dest.AppendFloat64(field.DataFloat64()[row]);
                    break;
                case FieldType::kBool:
                    dest.AppendBool(field.DataBool()[row] != 0);
                    break;
                case FieldType::kDictInt32:
                    dest.AppendDictInt32(field.DictionaryValue(field.DataDictIds()[row]));
                    break;
            }
        }
    }

    return result;
}

}  // namespace pcdb
