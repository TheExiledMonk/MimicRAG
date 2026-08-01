#include "mimicapi/core.h"

#include <algorithm>
#include <cstdlib>
#include <thread>

#include "mimicdb/aggregate.h"
#include "mimicdb/array_codec.h"
#include "mimicdb/compression.h"
#include "mimicdb/scan.h"
#include "mimicdb/vector_ivf.h"
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

size_t ApiThreadCount() {
    const char* env = std::getenv("MIMICDB_API_THREADS");
    if (!env || env[0] == '\0') {
        return 1;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(env, &end, 10);
    if (!end || *end != '\0' || value == 0) {
        return 1;
    }
    return static_cast<size_t>(value);
}

struct PreparedPredicate {
    Predicate pred;
    mimicdb::FieldType field_type = mimicdb::FieldType::kInt32;
    bool is_numeric = false;
    bool is_string = false;
    bool is_bytes = false;
};

bool PreparePredicates(const std::vector<mimicdb::FieldVector>& fields,
                       const std::vector<Predicate>& predicates,
                       std::vector<PreparedPredicate>* out) {
    if (!out) {
        return false;
    }
    out->clear();
    out->reserve(predicates.size());
    for (const auto& pred : predicates) {
        if (pred.field_index >= fields.size()) {
            return false;
        }
        const auto type = fields[pred.field_index].Type();
        PreparedPredicate prepared;
        prepared.pred = pred;
        prepared.field_type = type;
        prepared.is_numeric = IsNumeric(type);
        prepared.is_string = type == mimicdb::FieldType::kString;
        prepared.is_bytes = type == mimicdb::FieldType::kBytes;
        out->push_back(prepared);
    }
    return true;
}

void AccumulateAggregate(mimicapi::AggregateResult* dest,
                         const mimicapi::AggregateResult& src) {
    if (!dest) {
        return;
    }
    dest->count += src.count;
    dest->sum += src.sum;
    if (src.has_value) {
        if (!dest->has_value) {
            dest->min = src.min;
            dest->max = src.max;
            dest->has_value = true;
        } else {
            if (src.min < dest->min) {
                dest->min = src.min;
            }
            if (src.max > dest->max) {
                dest->max = src.max;
            }
        }
    }
}

void AccumulateAggregate(mimicapi::AggregateResult* dest,
                         const mimicdb::AggregateResult& src) {
    if (!dest) {
        return;
    }
    dest->count += src.count;
    dest->sum += src.sum;
    if (src.has_value) {
        if (!dest->has_value) {
            dest->min = src.min;
            dest->max = src.max;
            dest->has_value = true;
        } else {
            if (src.min < dest->min) {
                dest->min = src.min;
            }
            if (src.max > dest->max) {
                dest->max = src.max;
            }
        }
    }
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

bool MatchSingleEqPrepared(const std::vector<mimicdb::FieldVector>& fields, size_t index,
                           const PreparedPredicate& pred) {
    const auto& field = fields[pred.pred.field_index];
    if (pred.pred.is_null_check) {
        const bool is_null = !field.IsValid(index);
        return pred.pred.null_is == is_null;
    }
    if (pred.is_string || pred.is_bytes) {
        if (!field.IsValid(index)) {
            return false;
        }
        const std::string value = ReadVarlen(field, index);
        if (pred.pred.op == mimicdb::CompareOp::kEq) {
            return value == pred.pred.bytes;
        }
        return pred.pred.op == mimicdb::CompareOp::kNe && value != pred.pred.bytes;
    }
    double value = 0.0;
    if (!pred.is_numeric || !ReadNumeric(field, index, &value)) {
        return false;
    }
    return value == pred.pred.value;
}

bool MatchPredicatesPrepared(const std::vector<mimicdb::FieldVector>& fields, size_t index,
                             const std::vector<PreparedPredicate>& predicates) {
    if (predicates.size() == 1 &&
        predicates[0].pred.op == mimicdb::CompareOp::kEq &&
        !predicates[0].pred.is_null_check) {
        return MatchSingleEqPrepared(fields, index, predicates[0]);
    }
    for (const auto& pred : predicates) {
        if (pred.pred.field_index >= fields.size()) {
            return false;
        }
        const auto& field = fields[pred.pred.field_index];
        if (pred.pred.is_null_check) {
            const bool is_null = !field.IsValid(index);
            if (pred.pred.null_is != is_null) {
                return false;
            }
            continue;
        }
        if (pred.is_string || pred.is_bytes) {
            if (!field.IsValid(index)) {
                return false;
            }
            const std::string value = ReadVarlen(field, index);
            if (pred.pred.op == mimicdb::CompareOp::kEq && value != pred.pred.bytes) {
                return false;
            }
            if (pred.pred.op == mimicdb::CompareOp::kNe && value == pred.pred.bytes) {
                return false;
            }
            continue;
        }
        double value = 0.0;
        if (!pred.is_numeric || !ReadNumeric(field, index, &value)) {
            return false;
        }
        switch (pred.pred.op) {
            case mimicdb::CompareOp::kEq:
                if (!(value == pred.pred.value)) {
                    return false;
                }
                break;
            case mimicdb::CompareOp::kNe:
                if (!(value != pred.pred.value)) {
                    return false;
                }
                break;
            case mimicdb::CompareOp::kLt:
                if (!(value < pred.pred.value)) {
                    return false;
                }
                break;
            case mimicdb::CompareOp::kLe:
                if (!(value <= pred.pred.value)) {
                    return false;
                }
                break;
            case mimicdb::CompareOp::kGt:
                if (!(value > pred.pred.value)) {
                    return false;
                }
                break;
            case mimicdb::CompareOp::kGe:
                if (!(value >= pred.pred.value)) {
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
                    const std::vector<PreparedPredicate>& predicates, size_t* seen,
                    size_t offset, size_t limit, ScanResult* out) {
    std::vector<size_t> row_ids;
    row_ids.reserve(row_count);
    for (size_t i = 0; i < row_count; ++i) {
        if (!predicates.empty() && !MatchPredicatesPrepared(fields, i, predicates)) {
            continue;
        }
        if (*seen < offset) {
            (*seen)++;
            continue;
        }
        row_ids.push_back(i);
        (*seen)++;
        if (limit != 0 && out->rows.size() + row_ids.size() >= limit) {
            break;
        }
    }
    for (const auto row_id : row_ids) {
        if (limit != 0 && out->rows.size() >= limit) {
            return;
        }
        std::vector<mimicdb::FieldValue> row;
        row.reserve(column_indices.size());
        for (const auto index : column_indices) {
            row.push_back(ReadValue(fields[index], row_id));
        }
        out->rows.push_back(std::move(row));
    }
}

void AppendScanRowsCompressed(const std::vector<mimicdb::CompressedColumnView>& columns,
                              const std::vector<size_t>& column_indices, size_t row_count,
                              const std::vector<Predicate>& predicates, size_t* seen,
                              size_t offset, size_t limit, ScanResult* out) {
    bool has_mask = false;
    mimicdb::Mask mask;
    const bool single_eq =
        predicates.size() == 1 && predicates[0].op == mimicdb::CompareOp::kEq &&
        !predicates[0].is_null_check;
    if (!single_eq) {
        has_mask = BuildPredicateMaskCompressed(columns, predicates, &mask);
    }
    std::vector<size_t> row_ids;
    row_ids.reserve(row_count);
    for (size_t i = 0; i < row_count; ++i) {
        if (single_eq) {
            const auto& pred = predicates[0];
            if (pred.field_index >= columns.size()) {
                continue;
            }
            const auto& col = columns[pred.field_index];
            if (col.type == mimicdb::FieldType::kString ||
                col.type == mimicdb::FieldType::kBytes) {
                const std::string value = ReadVarlenCompressed(col, i);
                if (value != pred.bytes) {
                    continue;
                }
            } else {
                double value = 0.0;
                if (!ReadNumericCompressed(col, i, &value) || value != pred.value) {
                    continue;
                }
            }
        } else {
            if (has_mask && !mask.Get(i)) {
                continue;
            }
            if (!has_mask && !predicates.empty()) {
                continue;
            }
        }
        if (*seen < offset) {
            (*seen)++;
            continue;
        }
        row_ids.push_back(i);
        (*seen)++;
        if (limit != 0 && out->rows.size() + row_ids.size() >= limit) {
            break;
        }
    }
    for (const auto row_id : row_ids) {
        if (limit != 0 && out->rows.size() >= limit) {
            return;
        }
        std::vector<mimicdb::FieldValue> row;
        row.reserve(column_indices.size());
        for (const auto index : column_indices) {
            row.push_back(ReadValueCompressed(columns[index], row_id));
        }
        out->rows.push_back(std::move(row));
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

bool ApiClientCore::AddFields(const std::string& db, const std::string& name,
                              const std::vector<FieldDef>& fields,
                              std::string* error) {
    auto* state = GetDataset(db, name);
    if (!state) {
        if (error) {
            *error = "unknown dataset";
        }
        return false;
    }
    for (const auto& field : fields) {
        auto it = state->field_index.find(field.name);
        if (it != state->field_index.end()) {
            continue;
        }
        state->field_index[field.name] = state->fields.size();
        state->fields.push_back(field);
        state->dataset.AddField(mimicdb::FieldVector(field.name, field.type));
    }
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
    std::vector<PreparedPredicate> prepared_predicates;
    if (!PreparePredicates(state->dataset.Fields(), predicates, &prepared_predicates)) {
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
    std::vector<const mimicdb::Segment*> segments;
    segments.reserve(state->dataset.Segments().size() + 1);
    for (const auto& seg : state->dataset.Segments()) {
        segments.push_back(&seg);
    }
    mimicdb::Segment active_segment(0, 0, {});
    if (state->dataset.ActiveRowCount() > 0) {
        active_segment = mimicdb::Segment(state->dataset.SegmentCapacity(),
                                          state->dataset.ActiveRowCount(),
                                          state->dataset.ActiveFields());
        segments.push_back(&active_segment);
    }

    const size_t thread_count =
        std::min(ApiThreadCount(), segments.empty() ? size_t{1} : segments.size());
    if (thread_count <= 1 || segments.size() <= 1) {
        size_t seen = 0;
        for (const auto* segment : segments) {
            if (!SegmentCanMatchPredicates(*segment, predicates)) {
                result.rows_pruned += segment->RowCount();
                continue;
            }
            result.rows_scanned += segment->RowCount();
            if (segment->IsSealed() && !segment->CompressedColumns().empty()) {
                const auto& columns = segment->CompressedColumns();
                if (NeedsLz4Decode(columns)) {
                    static thread_local DecodedColumns decoded;
                    if (!BuildReadableColumns(columns, &decoded)) {
                        if (error) {
                            *error = "lz4 decode failed";
                        }
                        return ScanResult{};
                    }
                    AppendScanRowsCompressed(decoded.views, column_indices,
                                             segment->RowCount(), predicates, &seen,
                                             offset, limit, &result);
                } else {
                    AppendScanRowsCompressed(columns, column_indices,
                                             segment->RowCount(), predicates, &seen,
                                             offset, limit, &result);
                }
            } else {
                AppendScanRows(segment->Fields(), column_indices, segment->RowCount(),
                               prepared_predicates, &seen, offset, limit, &result);
            }
            if (limit != 0 && result.rows.size() >= limit) {
                return result;
            }
        }
        return result;
    }

    std::vector<std::vector<std::vector<mimicdb::FieldValue>>> segment_rows(segments.size());
    std::vector<uint8_t> should_scan(segments.size(), 0);
    for (size_t i = 0; i < segments.size(); ++i) {
        if (SegmentCanMatchPredicates(*segments[i], predicates)) {
            should_scan[i] = 1;
            result.rows_scanned += segments[i]->RowCount();
        } else {
            result.rows_pruned += segments[i]->RowCount();
        }
    }
    const auto schedule = mimicdb::ScheduleSegments(segments.size(), thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t]() {
            for (const auto seg_index : schedule[t]) {
                if (!should_scan[seg_index]) {
                    continue;
                }
                const auto* segment = segments[seg_index];
                ScanResult local_result;
                size_t local_seen = 0;
                if (segment->IsSealed() && !segment->CompressedColumns().empty()) {
                    const auto& columns = segment->CompressedColumns();
                    if (NeedsLz4Decode(columns)) {
                        static thread_local DecodedColumns decoded;
                        if (!BuildReadableColumns(columns, &decoded)) {
                            continue;
                        }
                        AppendScanRowsCompressed(decoded.views, column_indices,
                                                 segment->RowCount(), predicates, &local_seen,
                                                 0, 0, &local_result);
                    } else {
                        AppendScanRowsCompressed(columns, column_indices,
                                                 segment->RowCount(), predicates, &local_seen,
                                                 0, 0, &local_result);
                    }
                } else {
                    AppendScanRows(segment->Fields(), column_indices, segment->RowCount(),
                                   prepared_predicates, &local_seen, 0, 0, &local_result);
                }
                segment_rows[seg_index] = std::move(local_result.rows);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    size_t seen = 0;
    for (size_t seg_index = 0; seg_index < segment_rows.size(); ++seg_index) {
        auto& rows = segment_rows[seg_index];
        for (auto& row : rows) {
            if (seen < offset) {
                seen += 1;
                continue;
            }
            if (limit != 0 && result.rows.size() >= limit) {
                return result;
            }
            result.rows.push_back(std::move(row));
            seen += 1;
        }
    }
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
    std::vector<PreparedPredicate> prepared_predicates;
    if (!PreparePredicates(state->dataset.Fields(), predicates, &prepared_predicates)) {
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
    auto process_fields = [&](const std::vector<mimicdb::FieldVector>& fields, size_t row_count,
                              AggregateResult* out) {
        if (field_index >= fields.size()) {
            return;
        }
        const auto& field = fields[field_index];
        for (size_t i = 0; i < row_count; ++i) {
            out->rows_scanned += 1;
            if (!prepared_predicates.empty() &&
                !MatchPredicatesPrepared(fields, i, prepared_predicates)) {
                continue;
            }
            double value = 0.0;
            if (!IsNumeric(field.Type()) || !ReadNumeric(field, i, &value)) {
                continue;
            }
            out->count += 1;
            out->sum += value;
            if (!out->has_value) {
                out->min = value;
                out->max = value;
                out->has_value = true;
            } else {
                out->min = std::min(out->min, value);
                out->max = std::max(out->max, value);
            }
        }
    };
    const auto& segments = state->dataset.Segments();
    const size_t thread_count =
        std::min(ApiThreadCount(), segments.empty() ? size_t{1} : segments.size());
    if (thread_count <= 1 || segments.size() <= 1) {
        for (const auto& segment : segments) {
            if (!SegmentCanMatchPredicates(segment, predicates)) {
                result.rows_pruned += segment.RowCount();
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
                const bool single_eq =
                    predicates.size() == 1 && predicates[0].op == mimicdb::CompareOp::kEq &&
                    !predicates[0].is_null_check;
                mimicdb::AggregateResult local;
                if (single_eq) {
                    const auto& pred = predicates[0];
                    if (pred.field_index >= working_columns->size() ||
                        field_index >= working_columns->size()) {
                        continue;
                    }
                    const auto& pred_col = (*working_columns)[pred.field_index];
                    const auto& agg_col = (*working_columns)[field_index];
                    for (size_t i = 0; i < segment.RowCount(); ++i) {
                        bool keep = false;
                        if (pred_col.type == mimicdb::FieldType::kString ||
                            pred_col.type == mimicdb::FieldType::kBytes) {
                            keep = ReadVarlenCompressed(pred_col, i) == pred.bytes;
                        } else {
                            double value = 0.0;
                            keep = ReadNumericCompressed(pred_col, i, &value) &&
                                   value == pred.value;
                        }
                        if (!keep) {
                            continue;
                        }
                        double agg_value = 0.0;
                        if (!ReadNumericCompressed(agg_col, i, &agg_value)) {
                            continue;
                        }
                        local.count += 1;
                        local.sum += agg_value;
                        if (!local.has_value) {
                            local.min = agg_value;
                            local.max = agg_value;
                            local.has_value = true;
                        } else {
                            local.min = std::min(local.min, agg_value);
                            local.max = std::max(local.max, agg_value);
                        }
                    }
                } else {
                    mimicdb::Mask mask;
                    const mimicdb::Mask* mask_ptr = nullptr;
                    if (!predicates.empty()) {
                        if (!BuildPredicateMaskCompressed(*working_columns, predicates, &mask)) {
                            continue;
                        }
                        mask_ptr = &mask;
                    }
                    mimicdb::AggregateCompressed((*working_columns)[field_index], mask_ptr, &local);
                }
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
                process_fields(segment.Fields(), segment.RowCount(), &result);
            }
        }
    } else {
        const auto schedule = mimicdb::ScheduleSegments(segments.size(), thread_count);
        std::vector<AggregateResult> partial(thread_count);
        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (size_t t = 0; t < thread_count; ++t) {
            threads.emplace_back([&, t]() {
                for (const auto seg_index : schedule[t]) {
                    const auto& segment = segments[seg_index];
                    if (!SegmentCanMatchPredicates(segment, predicates)) {
                        partial[t].rows_pruned += segment.RowCount();
                        continue;
                    }
                    if (segment.IsSealed() && !segment.CompressedColumns().empty()) {
                        partial[t].rows_scanned += segment.RowCount();
                        const auto& columns = segment.CompressedColumns();
                        static thread_local DecodedColumns decoded;
                        const auto* working_columns = &columns;
                        if (NeedsLz4Decode(columns)) {
                            if (!BuildReadableColumns(columns, &decoded)) {
                                continue;
                            }
                            working_columns = &decoded.views;
                        }
                        if (field_index >= working_columns->size()) {
                            continue;
                        }
                        const bool single_eq =
                            predicates.size() == 1 && predicates[0].op == mimicdb::CompareOp::kEq &&
                            !predicates[0].is_null_check;
                        mimicdb::AggregateResult local;
                        if (single_eq) {
                            const auto& pred = predicates[0];
                            if (pred.field_index >= working_columns->size() ||
                                field_index >= working_columns->size()) {
                                continue;
                            }
                            const auto& pred_col = (*working_columns)[pred.field_index];
                            const auto& agg_col = (*working_columns)[field_index];
                            for (size_t i = 0; i < segment.RowCount(); ++i) {
                                bool keep = false;
                                if (pred_col.type == mimicdb::FieldType::kString ||
                                    pred_col.type == mimicdb::FieldType::kBytes) {
                                    keep = ReadVarlenCompressed(pred_col, i) == pred.bytes;
                                } else {
                                    double value = 0.0;
                                    keep = ReadNumericCompressed(pred_col, i, &value) &&
                                           value == pred.value;
                                }
                                if (!keep) {
                                    continue;
                                }
                                double agg_value = 0.0;
                                if (!ReadNumericCompressed(agg_col, i, &agg_value)) {
                                    continue;
                                }
                                local.count += 1;
                                local.sum += agg_value;
                                if (!local.has_value) {
                                    local.min = agg_value;
                                    local.max = agg_value;
                                    local.has_value = true;
                                } else {
                                    local.min = std::min(local.min, agg_value);
                                    local.max = std::max(local.max, agg_value);
                                }
                            }
                        } else {
                            mimicdb::Mask mask;
                            const mimicdb::Mask* mask_ptr = nullptr;
                            if (!predicates.empty()) {
                                if (!BuildPredicateMaskCompressed(*working_columns, predicates,
                                                                  &mask)) {
                                    continue;
                                }
                                mask_ptr = &mask;
                            }
                            mimicdb::AggregateCompressed((*working_columns)[field_index], mask_ptr,
                                                         &local);
                        }
                        if (local.count == 0) {
                            continue;
                        }
                        AccumulateAggregate(&partial[t], local);
                    } else {
                        process_fields(segment.Fields(), segment.RowCount(), &partial[t]);
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        for (const auto& part : partial) {
            result.rows_scanned += part.rows_scanned;
            result.rows_pruned += part.rows_pruned;
            AccumulateAggregate(&result, part);
        }
    }
    process_fields(state->dataset.ActiveFields(), state->dataset.ActiveRowCount(), &result);
    return result;
}

AggregateMultiResult ApiClientCore::AggregateMulti(const std::string& db,
                                                   const std::string& name,
                                                   const std::vector<AggregateRequest>& requests,
                                                   const std::vector<Predicate>& predicates,
                                                   std::string* error) const {
    AggregateMultiResult result;
    result.requests = requests;
    result.results.resize(requests.size());
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
    std::vector<PreparedPredicate> prepared_predicates;
    if (!PreparePredicates(state->dataset.Fields(), predicates, &prepared_predicates)) {
        if (error) {
            *error = "invalid predicate";
        }
        return result;
    }
    for (const auto& req : requests) {
        if (req.field_index >= state->dataset.Fields().size()) {
            if (error) {
                *error = "field_index out of range";
            }
            return result;
        }
        const auto type = state->dataset.Fields()[req.field_index].Type();
        if (!IsNumeric(type)) {
            if (error) {
                *error = "aggregate requires numeric field";
            }
            return result;
        }
    }
    auto process_fields = [&](const std::vector<mimicdb::FieldVector>& fields, size_t row_count,
                              std::vector<AggregateResult>* out, uint64_t* rows_scanned) {
        for (size_t i = 0; i < row_count; ++i) {
            *rows_scanned += 1;
            if (!prepared_predicates.empty() &&
                !MatchPredicatesPrepared(fields, i, prepared_predicates)) {
                continue;
            }
            for (size_t r = 0; r < requests.size(); ++r) {
                const auto& req = requests[r];
                const auto& field = fields[req.field_index];
                double value = 0.0;
                if (!IsNumeric(field.Type()) || !ReadNumeric(field, i, &value)) {
                    continue;
                }
                auto& agg = (*out)[r];
                agg.count += 1;
                agg.sum += value;
                if (!agg.has_value) {
                    agg.min = value;
                    agg.max = value;
                    agg.has_value = true;
                } else {
                    agg.min = std::min(agg.min, value);
                    agg.max = std::max(agg.max, value);
                }
            }
        }
    };

    const auto& segments = state->dataset.Segments();
    const size_t thread_count =
        std::min(ApiThreadCount(), segments.empty() ? size_t{1} : segments.size());
    if (thread_count <= 1 || segments.size() <= 1) {
        for (const auto& segment : segments) {
            if (!SegmentCanMatchPredicates(segment, predicates)) {
                result.rows_pruned += segment.RowCount();
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
                const bool single_eq =
                    predicates.size() == 1 && predicates[0].op == mimicdb::CompareOp::kEq &&
                    !predicates[0].is_null_check;
                if (single_eq && predicates[0].field_index < working_columns->size()) {
                    const auto& pred = predicates[0];
                    const auto& pred_col = (*working_columns)[pred.field_index];
                    for (size_t row = 0; row < segment.RowCount(); ++row) {
                        bool keep = false;
                        if (pred_col.type == mimicdb::FieldType::kString ||
                            pred_col.type == mimicdb::FieldType::kBytes) {
                            keep = ReadVarlenCompressed(pred_col, row) == pred.bytes;
                        } else {
                            double value = 0.0;
                            keep = ReadNumericCompressed(pred_col, row, &value) &&
                                   value == pred.value;
                        }
                        if (!keep) {
                            continue;
                        }
                        for (size_t r = 0; r < requests.size(); ++r) {
                            const auto& req = requests[r];
                            if (req.field_index >= working_columns->size()) {
                                continue;
                            }
                            double value = 0.0;
                            if (!ReadNumericCompressed((*working_columns)[req.field_index], row,
                                                       &value)) {
                                continue;
                            }
                            auto& agg = result.results[r];
                            agg.count += 1;
                            agg.sum += value;
                            if (!agg.has_value) {
                                agg.min = value;
                                agg.max = value;
                                agg.has_value = true;
                            } else {
                                agg.min = std::min(agg.min, value);
                                agg.max = std::max(agg.max, value);
                            }
                        }
                    }
                } else {
                    mimicdb::Mask mask;
                    const mimicdb::Mask* mask_ptr = nullptr;
                    if (!predicates.empty()) {
                        if (!BuildPredicateMaskCompressed(*working_columns, predicates, &mask)) {
                            continue;
                        }
                        mask_ptr = &mask;
                    }
                    for (size_t r = 0; r < requests.size(); ++r) {
                        const auto& req = requests[r];
                        if (req.field_index >= working_columns->size()) {
                            continue;
                        }
                        mimicdb::AggregateResult seg_result;
                        mimicdb::AggregateCompressed((*working_columns)[req.field_index], mask_ptr,
                                                     &seg_result);
                        AccumulateAggregate(&result.results[r], seg_result);
                    }
                }
            } else {
                process_fields(segment.Fields(), segment.RowCount(), &result.results,
                               &result.rows_scanned);
            }
        }
    } else {
        const auto schedule = mimicdb::ScheduleSegments(segments.size(), thread_count);
        std::vector<AggregateMultiResult> partial(thread_count);
        for (auto& part : partial) {
            part.results.resize(requests.size());
        }
        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (size_t t = 0; t < thread_count; ++t) {
            threads.emplace_back([&, t]() {
                for (const auto seg_index : schedule[t]) {
                    const auto& segment = segments[seg_index];
                    if (!SegmentCanMatchPredicates(segment, predicates)) {
                        partial[t].rows_pruned += segment.RowCount();
                        continue;
                    }
                    if (segment.IsSealed() && !segment.CompressedColumns().empty()) {
                        partial[t].rows_scanned += segment.RowCount();
                        const auto& columns = segment.CompressedColumns();
                        static thread_local DecodedColumns decoded;
                        const auto* working_columns = &columns;
                        if (NeedsLz4Decode(columns)) {
                            if (!BuildReadableColumns(columns, &decoded)) {
                                continue;
                            }
                            working_columns = &decoded.views;
                        }
                        const bool single_eq =
                            predicates.size() == 1 && predicates[0].op == mimicdb::CompareOp::kEq &&
                            !predicates[0].is_null_check;
                        if (single_eq && predicates[0].field_index < working_columns->size()) {
                            const auto& pred = predicates[0];
                            const auto& pred_col = (*working_columns)[pred.field_index];
                            for (size_t row = 0; row < segment.RowCount(); ++row) {
                                bool keep = false;
                                if (pred_col.type == mimicdb::FieldType::kString ||
                                    pred_col.type == mimicdb::FieldType::kBytes) {
                                    keep = ReadVarlenCompressed(pred_col, row) == pred.bytes;
                                } else {
                                    double value = 0.0;
                                    keep = ReadNumericCompressed(pred_col, row, &value) &&
                                           value == pred.value;
                                }
                                if (!keep) {
                                    continue;
                                }
                                for (size_t r = 0; r < requests.size(); ++r) {
                                    const auto& req = requests[r];
                                    if (req.field_index >= working_columns->size()) {
                                        continue;
                                    }
                                    double value = 0.0;
                                    if (!ReadNumericCompressed((*working_columns)[req.field_index],
                                                               row, &value)) {
                                        continue;
                                    }
                                    auto& agg = partial[t].results[r];
                                    agg.count += 1;
                                    agg.sum += value;
                                    if (!agg.has_value) {
                                        agg.min = value;
                                        agg.max = value;
                                        agg.has_value = true;
                                    } else {
                                        agg.min = std::min(agg.min, value);
                                        agg.max = std::max(agg.max, value);
                                    }
                                }
                            }
                        } else {
                            mimicdb::Mask mask;
                            const mimicdb::Mask* mask_ptr = nullptr;
                            if (!predicates.empty()) {
                                if (!BuildPredicateMaskCompressed(*working_columns, predicates,
                                                                  &mask)) {
                                    continue;
                                }
                                mask_ptr = &mask;
                            }
                            for (size_t r = 0; r < requests.size(); ++r) {
                                const auto& req = requests[r];
                                if (req.field_index >= working_columns->size()) {
                                    continue;
                                }
                                mimicdb::AggregateResult seg_result;
                                mimicdb::AggregateCompressed((*working_columns)[req.field_index],
                                                             mask_ptr, &seg_result);
                                AccumulateAggregate(&partial[t].results[r], seg_result);
                            }
                        }
                    } else {
                        process_fields(segment.Fields(), segment.RowCount(), &partial[t].results,
                                       &partial[t].rows_scanned);
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        for (const auto& part : partial) {
            result.rows_scanned += part.rows_scanned;
            result.rows_pruned += part.rows_pruned;
            for (size_t r = 0; r < requests.size(); ++r) {
                AccumulateAggregate(&result.results[r], part.results[r]);
            }
        }
    }
    process_fields(state->dataset.ActiveFields(), state->dataset.ActiveRowCount(),
                   &result.results, &result.rows_scanned);
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

bool ApiClientCore::VectorSearch(
    const std::string& db, const std::string& name, size_t field_index,
    const std::vector<float>& query, size_t top_k, mimicdb::VectorMetric metric,
    const std::vector<mimicdb::VectorSearchPredicate>& predicates,
    bool approximate, size_t probes,
    std::vector<mimicdb::VectorSearchHit>* out, std::string* error) const {
    const auto* state = GetDataset(db, name);
    if (!state) { if (error) *error = "unknown dataset"; return false; }
    const bool ok = approximate
        ? mimicdb::VectorSearchIvf(state->dataset, field_index, query.data(), query.size(),
                                   top_k, metric, probes, out, predicates)
        : mimicdb::VectorSearch(state->dataset, field_index, query.data(), query.size(),
                                top_k, metric, out, predicates);
    if (!ok) {
        if (error) *error = "invalid vector search";
        return false;
    }
    return true;
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
