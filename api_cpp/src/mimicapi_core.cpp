#include "mimicapi/core.h"

#include <algorithm>

#include "mimicdb/array_codec.h"
namespace mimicapi {
namespace {

bool IsNumeric(mimicdb::FieldType type) {
    return type == mimicdb::FieldType::kInt32 ||
           type == mimicdb::FieldType::kInt64 ||
           type == mimicdb::FieldType::kFloat64 ||
           type == mimicdb::FieldType::kBool ||
           type == mimicdb::FieldType::kDictInt32;
}

bool IsComparable(mimicdb::FieldType type) {
    return IsNumeric(type) || type == mimicdb::FieldType::kString ||
           type == mimicdb::FieldType::kBytes;
}

bool ReadNumeric(const mimicdb::FieldVector& field, size_t index, double* out) {
    if (!field.IsValid(index)) {
        return false;
    }
    switch (field.Type()) {
        case mimicdb::FieldType::kInt32:
            *out = static_cast<double>(field.DataInt32()[index]);
            return true;
        case mimicdb::FieldType::kInt64:
            *out = static_cast<double>(field.DataInt64()[index]);
            return true;
        case mimicdb::FieldType::kFloat64:
            *out = field.DataFloat64()[index];
            return true;
        case mimicdb::FieldType::kBool:
            *out = static_cast<double>(field.DataBool()[index] != 0);
            return true;
        case mimicdb::FieldType::kDictInt32:
            *out = static_cast<double>(field.DictionaryValue(field.DataDictIds()[index]));
            return true;
        default:
            return false;
    }
}

std::string ReadVarlen(const mimicdb::FieldVector& field, size_t index);

mimicdb::FieldValue ReadValue(const mimicdb::FieldVector& field, size_t index) {
    if (!field.IsValid(index)) {
        return mimicdb::FieldValue::Null(field.Type());
    }
    switch (field.Type()) {
        case mimicdb::FieldType::kInt32:
            return mimicdb::FieldValue::Int32(field.DataInt32()[index]);
        case mimicdb::FieldType::kInt64:
            return mimicdb::FieldValue::Int64(field.DataInt64()[index]);
        case mimicdb::FieldType::kFloat64:
            return mimicdb::FieldValue::Float64(field.DataFloat64()[index]);
        case mimicdb::FieldType::kBool:
            return mimicdb::FieldValue::Bool(field.DataBool()[index] != 0);
        case mimicdb::FieldType::kDictInt32:
            return mimicdb::FieldValue::Int32(field.DictionaryValue(field.DataDictIds()[index]));
        case mimicdb::FieldType::kString: {
            const uint32_t* lengths = field.DataLengths();
            const uint8_t* bytes = field.DataBytes();
            size_t offset = 0;
            for (size_t i = 0; i < index; ++i) {
                offset += lengths[i];
            }
            return mimicdb::FieldValue::String(
                std::string(reinterpret_cast<const char*>(bytes + offset), lengths[index]));
        }
        case mimicdb::FieldType::kBytes: {
            const uint32_t* lengths = field.DataLengths();
            const uint8_t* bytes = field.DataBytes();
            size_t offset = 0;
            for (size_t i = 0; i < index; ++i) {
                offset += lengths[i];
            }
            return mimicdb::FieldValue::Bytes(
                std::string(reinterpret_cast<const char*>(bytes + offset), lengths[index]));
        }
        case mimicdb::FieldType::kArray: {
            const std::string encoded = ReadVarlen(field, index);
            std::vector<mimicdb::FieldValue> values;
            if (!mimicdb::DecodeArray(encoded, &values)) {
                return mimicdb::FieldValue::Null(mimicdb::FieldType::kArray);
            }
            return mimicdb::FieldValue::Array(values);
        }
        case mimicdb::FieldType::kObject:
            return mimicdb::FieldValue::Null(mimicdb::FieldType::kObject);
    }
    return mimicdb::FieldValue::Null(field.Type());
}

std::string ReadVarlen(const mimicdb::FieldVector& field, size_t index) {
    const uint32_t* lengths = field.DataLengths();
    const uint32_t* offsets = field.DataOffsets();
    const uint8_t* bytes = field.DataBytes();
    if (!lengths || !offsets || !bytes) {
        return {};
    }
    const uint32_t offset = offsets[index];
    return std::string(reinterpret_cast<const char*>(bytes + offset), lengths[index]);
}

bool ValidatePredicates(const std::vector<mimicdb::FieldVector>& fields,
                        const std::vector<Predicate>& predicates) {
    for (const auto& pred : predicates) {
        if (pred.field_index >= fields.size()) {
            return false;
        }
        const auto type = fields[pred.field_index].Type();
        if (pred.is_null_check) {
            continue;
        }
        if (!IsComparable(type)) {
            return false;
        }
        if (type != pred.value_type) {
            if (!(IsNumeric(type) && IsNumeric(pred.value_type))) {
                return false;
            }
        }
        if ((type == mimicdb::FieldType::kString || type == mimicdb::FieldType::kBytes) &&
            !(pred.op == mimicdb::CompareOp::kEq || pred.op == mimicdb::CompareOp::kNe)) {
            return false;
        }
    }
    return true;
}

bool MatchPredicates(const std::vector<mimicdb::FieldVector>& fields, size_t index,
                     const std::vector<Predicate>& predicates) {
    for (const auto& pred : predicates) {
        if (pred.field_index >= fields.size()) {
            return false;
        }
        const auto& field = fields[pred.field_index];
        if (pred.is_null_check) {
            const bool is_null = !field.IsValid(index);
            if (pred.null_is != is_null) {
                return false;
            }
            continue;
        }
        if (field.Type() == mimicdb::FieldType::kString ||
            field.Type() == mimicdb::FieldType::kBytes) {
            if (!field.IsValid(index)) {
                return false;
            }
            const std::string value = ReadVarlen(field, index);
            if (pred.op == mimicdb::CompareOp::kEq && value != pred.bytes) {
                return false;
            }
            if (pred.op == mimicdb::CompareOp::kNe && value == pred.bytes) {
                return false;
            }
            continue;
        }
        double value = 0.0;
        if (!IsNumeric(field.Type()) || !ReadNumeric(field, index, &value)) {
            return false;
        }
        switch (pred.op) {
            case mimicdb::CompareOp::kEq:
                if (!(value == pred.value)) {
                    return false;
                }
                break;
            case mimicdb::CompareOp::kNe:
                if (!(value != pred.value)) {
                    return false;
                }
                break;
            case mimicdb::CompareOp::kLt:
                if (!(value < pred.value)) {
                    return false;
                }
                break;
            case mimicdb::CompareOp::kLe:
                if (!(value <= pred.value)) {
                    return false;
                }
                break;
            case mimicdb::CompareOp::kGt:
                if (!(value > pred.value)) {
                    return false;
                }
                break;
            case mimicdb::CompareOp::kGe:
                if (!(value >= pred.value)) {
                    return false;
                }
                break;
        }
    }
    return true;
}

void AppendScanRows(const std::vector<mimicdb::FieldVector>& fields,
                    const std::vector<size_t>& column_indices, size_t row_count,
                    const std::vector<Predicate>& predicates, size_t* seen,
                    size_t offset, size_t limit, ScanResult* out) {
    for (size_t i = 0; i < row_count; ++i) {
        if (!predicates.empty() && !MatchPredicates(fields, i, predicates)) {
            continue;
        }
        if (*seen < offset) {
            (*seen)++;
            continue;
        }
        if (limit != 0 && out->rows.size() >= limit) {
            return;
        }
        std::vector<mimicdb::FieldValue> row;
        row.reserve(column_indices.size());
        for (const auto index : column_indices) {
            row.push_back(ReadValue(fields[index], i));
        }
        out->rows.push_back(std::move(row));
        (*seen)++;
    }
}

}  // namespace

bool ApiClientCore::CreateDatabase(const std::string& name) {
    databases_.try_emplace(name);
    return true;
}

bool ApiClientCore::CreateDataset(const std::string& db, const std::string& name,
                                  const std::vector<FieldDef>& fields) {
    CreateDatabase(db);
    auto& db_state = databases_[db];
    if (db_state.find(name) != db_state.end()) {
        return false;
    }
    DatasetState state(name);
    state.fields = fields;
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto& field = fields[i];
        state.field_index[field.name] = i;
        state.dataset.AddField(mimicdb::FieldVector(field.name, field.type));
    }
    db_state.emplace(name, std::move(state));
    return true;
}

bool ApiClientCore::DropDatabase(const std::string& name) {
    return databases_.erase(name) > 0;
}

bool ApiClientCore::DropDataset(const std::string& db, const std::string& name) {
    auto it = databases_.find(db);
    if (it == databases_.end()) {
        return false;
    }
    return it->second.erase(name) > 0;
}

const std::vector<FieldDef>* ApiClientCore::FieldsFor(const std::string& db,
                                                      const std::string& name) const {
    const auto* state = GetDataset(db, name);
    if (!state) {
        return nullptr;
    }
    return &state->fields;
}

bool ApiClientCore::AppendBatch(const std::string& db, const std::string& name,
                                const std::vector<mimicdb::FieldBatch>& batches,
                                std::string* error) {
    auto* state = GetDataset(db, name);
    if (!state) {
        if (error) {
            *error = "unknown dataset";
        }
        return false;
    }
    if (!state->dataset.AppendBatch(batches)) {
        if (error) {
            *error = "append_batch failed";
        }
        return false;
    }
    return true;
}

ScanResult ApiClientCore::Scan(const std::string& db, const std::string& name,
                               const std::vector<std::string>& columns,
                               const std::vector<Predicate>& predicates, size_t limit,
                               size_t offset, std::string* error) const {
    ScanResult result;
    const auto* state = GetDataset(db, name);
    if (!state) {
        if (error) {
            *error = "unknown dataset";
        }
        return result;
    }
    if (!ValidatePredicates(state->dataset.Fields(), predicates)) {
        if (error) {
            *error = "invalid predicate";
        }
        return result;
    }
    std::vector<size_t> column_indices;
    if (columns.empty()) {
        result.columns.reserve(state->fields.size());
        for (size_t i = 0; i < state->fields.size(); ++i) {
            result.columns.push_back(state->fields[i].name);
            column_indices.push_back(i);
        }
    } else {
        for (const auto& name : columns) {
            auto it = state->field_index.find(name);
            if (it != state->field_index.end()) {
                result.columns.push_back(name);
                column_indices.push_back(it->second);
            } else if (error) {
                *error = "unknown column";
                return ScanResult{};
            }
        }
    }
    size_t seen = 0;
    for (const auto& segment : state->dataset.Segments()) {
        AppendScanRows(segment.Fields(), column_indices, segment.RowCount(),
                       predicates, &seen, offset, limit, &result);
        if (limit != 0 && result.rows.size() >= limit) {
            return result;
        }
    }
    const auto& active = state->dataset.ActiveFields();
    AppendScanRows(active, column_indices, state->dataset.ActiveRowCount(),
                   predicates, &seen, offset, limit, &result);
    return result;
}

AggregateResult ApiClientCore::Aggregate(const std::string& db, const std::string& name,
                                         size_t field_index,
                                         const std::vector<Predicate>& predicates,
                                         std::string* error) const {
    AggregateResult result;
    const auto* state = GetDataset(db, name);
    if (!state) {
        if (error) {
            *error = "unknown dataset";
        }
        return result;
    }
    if (!ValidatePredicates(state->dataset.Fields(), predicates)) {
        if (error) {
            *error = "invalid predicate";
        }
        return result;
    }
    if (field_index >= state->dataset.Fields().size()) {
        if (error) {
            *error = "field_index out of range";
        }
        return result;
    }
    const auto agg_type = state->dataset.Fields()[field_index].Type();
    if (!IsNumeric(agg_type)) {
        if (error) {
            *error = "aggregate requires numeric field";
        }
        return result;
    }
    auto process_fields = [&](const std::vector<mimicdb::FieldVector>& fields, size_t row_count) {
        if (field_index >= fields.size()) {
            return;
        }
        const auto& field = fields[field_index];
        for (size_t i = 0; i < row_count; ++i) {
            result.rows_scanned += 1;
            if (!predicates.empty() && !MatchPredicates(fields, i, predicates)) {
                continue;
            }
            double value = 0.0;
            if (!IsNumeric(field.Type()) || !ReadNumeric(field, i, &value)) {
                continue;
            }
            result.count += 1;
            result.sum += value;
            if (!result.has_value) {
                result.min = value;
                result.max = value;
                result.has_value = true;
            } else {
                result.min = std::min(result.min, value);
                result.max = std::max(result.max, value);
            }
        }
    };
    for (const auto& segment : state->dataset.Segments()) {
        process_fields(segment.Fields(), segment.RowCount());
    }
    process_fields(state->dataset.ActiveFields(), state->dataset.ActiveRowCount());
    return result;
}

const ApiClientCore::DatasetState* ApiClientCore::GetDataset(const std::string& db,
                                                             const std::string& name) const {
    auto db_it = databases_.find(db);
    if (db_it == databases_.end()) {
        return nullptr;
    }
    auto it = db_it->second.find(name);
    if (it == db_it->second.end()) {
        return nullptr;
    }
    return &it->second;
}

ApiClientCore::DatasetState* ApiClientCore::GetDataset(const std::string& db,
                                                       const std::string& name) {
    auto db_it = databases_.find(db);
    if (db_it == databases_.end()) {
        return nullptr;
    }
    auto it = db_it->second.find(name);
    if (it == db_it->second.end()) {
        return nullptr;
    }
    return &it->second;
}

}  // namespace mimicapi
