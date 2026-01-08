#include "mimicapi/core.h"

#include <algorithm>

#include "mimicdb/aggregate.h"
#include "mimicdb/array_codec.h"
#include "mimicdb/compression.h"
namespace mimicapi {
namespace {

struct DecodedColumns {
    std::vector<mimicdb::CompressedColumnView> views;
    std::vector<std::vector<uint8_t>> data;
    std::vector<std::vector<uint8_t>> aux;
};

bool NeedsLz4Decode(const std::vector<mimicdb::CompressedColumnView>& columns) {
    for (const auto& col : columns) {
        if (col.kind == mimicdb::ColumnCompressionKind::kLz4) {
            return true;
        }
    }
    return false;
}

bool BuildReadableColumns(const std::vector<mimicdb::CompressedColumnView>& columns,
                          DecodedColumns* out) {
    if (!out) {
        return false;
    }
    out->views.clear();
    out->views.reserve(columns.size());
    if (out->data.size() < columns.size()) {
        out->data.resize(columns.size());
    }
    if (out->aux.size() < columns.size()) {
        out->aux.resize(columns.size());
    }
    for (size_t i = 0; i < columns.size(); ++i) {
        out->data[i].clear();
        out->aux[i].clear();
        const auto& col = columns[i];
        if (col.kind == mimicdb::ColumnCompressionKind::kLz4) {
            if (col.raw_data_size > 0) {
                out->data[i].resize(col.raw_data_size);
                if (!mimicdb::DecodeLz4Literal(col.data, col.data_size,
                                               out->data[i].data(),
                                               out->data[i].size())) {
                    return false;
                }
            }
            if (col.raw_aux_size > 0) {
                out->aux[i].resize(col.raw_aux_size);
                if (!mimicdb::DecodeLz4Literal(col.aux, col.aux_size,
                                               out->aux[i].data(),
                                               out->aux[i].size())) {
                    return false;
                }
            }
            mimicdb::CompressedColumnView view = col;
            view.kind = mimicdb::ColumnCompressionKind::kNone;
            view.data = out->data[i].empty() ? nullptr : out->data[i].data();
            view.data_size = col.raw_data_size;
            view.aux = out->aux[i].empty() ? nullptr : out->aux[i].data();
            view.aux_size = col.raw_aux_size;
            out->views.push_back(view);
        } else {
            out->views.push_back(col);
        }
    }
    return true;
}

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

bool SegmentCanMatchPredicates(const mimicdb::Segment& segment,
                               const std::vector<Predicate>& predicates) {
    if (predicates.empty()) {
        return true;
    }
    const auto& stats = segment.ColumnStats();
    const auto& fields = segment.Fields();
    for (const auto& pred : predicates) {
        if (pred.field_index >= stats.size() || pred.field_index >= fields.size()) {
            continue;
        }
        const auto& col_stats = stats[pred.field_index];
        if (pred.is_null_check) {
            if (pred.null_is) {
                if (col_stats.null_count == 0) {
                    return false;
                }
            } else if (col_stats.value_count > 0 &&
                       col_stats.null_count >= col_stats.value_count) {
                return false;
            }
            continue;
        }
        const auto type = fields[pred.field_index].Type();
        if (!IsNumeric(type)) {
            continue;
        }
        if (!col_stats.has_value) {
            continue;
        }
        if (!mimicdb::PredicateCanMatchRange(col_stats.min, col_stats.max, pred.op,
                                             pred.value)) {
            return false;
        }
    }
    return true;
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

bool ReadNumericCompressed(const mimicdb::CompressedColumnView& column, size_t index, double* out) {
    return mimicdb::ReadNumericValue(column, index, out);
}

std::string ReadVarlen(const mimicdb::FieldVector& field, size_t index);
std::string ReadVarlenCompressed(const mimicdb::CompressedColumnView& column, size_t index);

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

std::string ReadVarlenCompressed(const mimicdb::CompressedColumnView& column, size_t index) {
    const auto* lengths = reinterpret_cast<const uint32_t*>(column.aux);
    const auto* bytes = column.data;
    if (!lengths || !bytes) {
        return {};
    }
    uint32_t offset = 0;
    for (size_t i = 0; i < index; ++i) {
        offset += lengths[i];
    }
    return std::string(reinterpret_cast<const char*>(bytes + offset), lengths[index]);
}

mimicdb::FieldValue ReadValueCompressed(const mimicdb::CompressedColumnView& column, size_t index) {
    if (!mimicdb::IsValid(column, index)) {
        return mimicdb::FieldValue::Null(column.type);
    }
    switch (column.type) {
        case mimicdb::FieldType::kInt32: {
            int64_t value = 0;
            if (!mimicdb::ReadInt64Value(column, index, &value)) {
                return mimicdb::FieldValue::Null(column.type);
            }
            return mimicdb::FieldValue::Int32(static_cast<int32_t>(value));
        }
        case mimicdb::FieldType::kInt64: {
            int64_t value = 0;
            if (!mimicdb::ReadInt64Value(column, index, &value)) {
                return mimicdb::FieldValue::Null(column.type);
            }
            return mimicdb::FieldValue::Int64(value);
        }
        case mimicdb::FieldType::kFloat64: {
            double value = 0.0;
            if (!mimicdb::ReadNumericValue(column, index, &value)) {
                return mimicdb::FieldValue::Null(column.type);
            }
            return mimicdb::FieldValue::Float64(value);
        }
        case mimicdb::FieldType::kBool: {
            int64_t value = 0;
            if (!mimicdb::ReadInt64Value(column, index, &value)) {
                return mimicdb::FieldValue::Null(column.type);
            }
            return mimicdb::FieldValue::Bool(value != 0);
        }
        case mimicdb::FieldType::kDictInt32: {
            int64_t value = 0;
            if (!mimicdb::ReadInt64Value(column, index, &value)) {
                return mimicdb::FieldValue::Null(column.type);
            }
            return mimicdb::FieldValue::Int32(static_cast<int32_t>(value));
        }
        case mimicdb::FieldType::kString: {
            return mimicdb::FieldValue::String(ReadVarlenCompressed(column, index));
        }
        case mimicdb::FieldType::kBytes: {
            return mimicdb::FieldValue::Bytes(ReadVarlenCompressed(column, index));
        }
        case mimicdb::FieldType::kArray: {
            const std::string encoded = ReadVarlenCompressed(column, index);
            std::vector<mimicdb::FieldValue> values;
            if (!mimicdb::DecodeArray(encoded, &values)) {
                return mimicdb::FieldValue::Null(mimicdb::FieldType::kArray);
            }
            return mimicdb::FieldValue::Array(values);
        }
        case mimicdb::FieldType::kObject:
            return mimicdb::FieldValue::Null(mimicdb::FieldType::kObject);
    }
    return mimicdb::FieldValue::Null(column.type);
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

bool BuildPredicateMaskCompressed(const std::vector<mimicdb::CompressedColumnView>& columns,
                                  const std::vector<Predicate>& predicates,
                                  mimicdb::Mask* out_mask) {
    if (!out_mask || predicates.empty()) {
        return false;
    }
    bool has_mask = false;
    mimicdb::Mask mask;
    for (const auto& pred : predicates) {
        if (pred.field_index >= columns.size()) {
            return false;
        }
        const auto& col = columns[pred.field_index];
        mimicdb::Mask current;
        if (col.ops && col.ops->scan_predicate &&
            col.type != mimicdb::FieldType::kString &&
            col.type != mimicdb::FieldType::kBytes) {
            mimicdb::CompressionPredicate cpred;
            cpred.type = col.type;
            cpred.op = pred.op;
            cpred.is_null_check = pred.is_null_check;
            cpred.is_not_null_check = pred.is_null_check && !pred.null_is;
            cpred.null_is = pred.null_is;
            cpred.i64 = static_cast<int64_t>(pred.value);
            cpred.f64 = pred.value;
            mimicdb::BuildMaskCompressed(col, cpred, &current);
        } else if (pred.is_null_check) {
            current.Resize(col.row_count);
            for (size_t i = 0; i < col.row_count; ++i) {
                const bool is_null = !mimicdb::IsValid(col, i);
                current.Set(i, pred.null_is ? is_null : !is_null);
            }
        } else if (col.type == mimicdb::FieldType::kString ||
                   col.type == mimicdb::FieldType::kBytes) {
            current.Resize(col.row_count);
            for (size_t i = 0; i < col.row_count; ++i) {
                if (!mimicdb::IsValid(col, i)) {
                    current.Set(i, false);
                    continue;
                }
                const std::string value = ReadVarlenCompressed(col, i);
                bool keep = false;
                if (pred.op == mimicdb::CompareOp::kEq) {
                    keep = value == pred.bytes;
                } else if (pred.op == mimicdb::CompareOp::kNe) {
                    keep = value != pred.bytes;
                }
                current.Set(i, keep);
            }
        } else {
            current.Resize(col.row_count);
            for (size_t i = 0; i < col.row_count; ++i) {
                double value = 0.0;
                if (!ReadNumericCompressed(col, i, &value)) {
                    current.Set(i, false);
                    continue;
                }
                bool keep = false;
                switch (pred.op) {
                    case mimicdb::CompareOp::kEq:
                        keep = value == pred.value;
                        break;
                    case mimicdb::CompareOp::kNe:
                        keep = value != pred.value;
                        break;
                    case mimicdb::CompareOp::kLt:
                        keep = value < pred.value;
                        break;
                    case mimicdb::CompareOp::kLe:
                        keep = value <= pred.value;
                        break;
                    case mimicdb::CompareOp::kGt:
                        keep = value > pred.value;
                        break;
                    case mimicdb::CompareOp::kGe:
                        keep = value >= pred.value;
                        break;
                }
                current.Set(i, keep);
            }
        }
        if (!has_mask) {
            mask = std::move(current);
            has_mask = true;
        } else {
            mask = mimicdb::Mask::And(mask, current);
        }
    }
    *out_mask = std::move(mask);
    return has_mask;
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

void AppendScanRowsCompressed(const std::vector<mimicdb::CompressedColumnView>& columns,
                              const std::vector<size_t>& column_indices, size_t row_count,
                              const std::vector<Predicate>& predicates, size_t* seen,
                              size_t offset, size_t limit, ScanResult* out) {
    mimicdb::Mask mask;
    const bool has_mask = BuildPredicateMaskCompressed(columns, predicates, &mask);
    for (size_t i = 0; i < row_count; ++i) {
        if (has_mask && !mask.Get(i)) {
            continue;
        }
        if (!has_mask && !predicates.empty()) {
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
            row.push_back(ReadValueCompressed(columns[index], i));
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
        if (!SegmentCanMatchPredicates(segment, predicates)) {
            continue;
        }
        if (segment.IsSealed() && !segment.CompressedColumns().empty()) {
            const auto& columns = segment.CompressedColumns();
            if (NeedsLz4Decode(columns)) {
                static thread_local DecodedColumns decoded;
                if (!BuildReadableColumns(columns, &decoded)) {
                    if (error) {
                        *error = "lz4 decode failed";
                    }
                    return ScanResult{};
                }
                AppendScanRowsCompressed(decoded.views, column_indices,
                                         segment.RowCount(), predicates, &seen,
                                         offset, limit, &result);
            } else {
                AppendScanRowsCompressed(columns, column_indices,
                                         segment.RowCount(), predicates, &seen,
                                         offset, limit, &result);
            }
        } else {
            AppendScanRows(segment.Fields(), column_indices, segment.RowCount(),
                           predicates, &seen, offset, limit, &result);
        }
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
        if (!SegmentCanMatchPredicates(segment, predicates)) {
            continue;
        }
        if (segment.IsSealed() && !segment.CompressedColumns().empty()) {
            result.rows_scanned += segment.RowCount();
            const auto& columns = segment.CompressedColumns();
            static thread_local DecodedColumns decoded;
            const auto* working_columns = &columns;
            if (NeedsLz4Decode(columns)) {
                if (!BuildReadableColumns(columns, &decoded)) {
                    if (error) {
                        *error = "lz4 decode failed";
                    }
                    return result;
                }
                working_columns = &decoded.views;
            }
            if (field_index >= working_columns->size()) {
                continue;
            }
            mimicdb::Mask mask;
            const mimicdb::Mask* mask_ptr = nullptr;
            if (!predicates.empty()) {
                if (!BuildPredicateMaskCompressed(*working_columns, predicates, &mask)) {
                    continue;
                }
                mask_ptr = &mask;
            }
            mimicdb::AggregateResult local;
            mimicdb::AggregateCompressed((*working_columns)[field_index], mask_ptr, &local);
            if (local.count == 0) {
                continue;
            }
            result.count += local.count;
            result.sum += local.sum;
            if (!result.has_value && local.has_value) {
                result.min = local.min;
                result.max = local.max;
                result.has_value = true;
            } else if (local.has_value) {
                result.min = std::min(result.min, local.min);
                result.max = std::max(result.max, local.max);
            }
        } else {
            process_fields(segment.Fields(), segment.RowCount());
        }
    }
    process_fields(state->dataset.ActiveFields(), state->dataset.ActiveRowCount());
    return result;
}

CompressionStats ApiClientCore::CompressionStatsFor(const std::string& db,
                                                    const std::string& name,
                                                    std::string* error) const {
    CompressionStats stats;
    const auto* state = GetDataset(db, name);
    if (!state) {
        if (error) {
            *error = "unknown dataset";
        }
        return stats;
    }
    const auto ds_stats = state->dataset.CompressionStats();
    stats.raw_bytes = ds_stats.raw_bytes;
    stats.compressed_bytes = ds_stats.compressed_bytes;
    stats.segments = ds_stats.segments;
    stats.compressed_segments = ds_stats.compressed_segments;
    stats.compressed_columns = ds_stats.compressed_columns;
    stats.active_rows = ds_stats.active_rows;
    return stats;
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
