#include "mimicdb/aggregate.h"

#include "mimicdb/scan.h"

#include <cstddef>
#include <thread>
#include <vector>

namespace mimicdb {

namespace {

struct DecodedColumns {
    std::vector<CompressedColumnView> views;
    std::vector<std::vector<uint8_t>> data;
    std::vector<std::vector<uint8_t>> aux;
};

bool NeedsLz4Decode(const std::vector<CompressedColumnView>& columns) {
    for (const auto& col : columns) {
        if (col.kind == ColumnCompressionKind::kLz4) {
            return true;
        }
    }
    return false;
}

bool BuildReadableColumns(const std::vector<CompressedColumnView>& columns,
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
        if (col.kind == ColumnCompressionKind::kLz4) {
            if (col.raw_data_size > 0) {
                out->data[i].resize(col.raw_data_size);
                if (!DecodeLz4Literal(col.data, col.data_size,
                                      out->data[i].data(),
                                      out->data[i].size())) {
                    return false;
                }
            }
            if (col.raw_aux_size > 0) {
                out->aux[i].resize(col.raw_aux_size);
                if (!DecodeLz4Literal(col.aux, col.aux_size,
                                      out->aux[i].data(),
                                      out->aux[i].size())) {
                    return false;
                }
            }
            CompressedColumnView view = col;
            view.kind = ColumnCompressionKind::kNone;
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

void AccumulateAggregate(AggregateResult* dest, const AggregateResult& src) {
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

}  // namespace

void AggregateDouble(const double* values, size_t count, const Mask* mask,
                     AggregateResult* out) {
    if (!out || !values) {
        return;
    }
    AggregateResult result;
    for (size_t i = 0; i < count; ++i) {
        if (mask && !mask->Get(i)) {
            continue;
        }
        const double value = values[i];
        result.sum += value;
        if (!result.has_value) {
            result.min = value;
            result.max = value;
            result.has_value = true;
        } else {
            if (value < result.min) {
                result.min = value;
            }
            if (value > result.max) {
                result.max = value;
            }
        }
        ++result.count;
    }
    *out = result;
}

void AggregateField(const FieldVector& field, const Mask* mask, AggregateResult* out) {
    if (!out) {
        return;
    }
    const size_t count = field.Size();
    AggregateResult result;
    const bool has_nulls = field.HasNulls();
    switch (field.Type()) {
        case FieldType::kInt32: {
            const auto* values = field.DataInt32();
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                const double value = static_cast<double>(values[i]);
                result.sum += value;
                if (!result.has_value) {
                    result.min = value;
                    result.max = value;
                    result.has_value = true;
                } else {
                    if (value < result.min) {
                        result.min = value;
                    }
                    if (value > result.max) {
                        result.max = value;
                    }
                }
                ++result.count;
            }
            break;
        }
        case FieldType::kInt64: {
            const auto* values = field.DataInt64();
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                const double value = static_cast<double>(values[i]);
                result.sum += value;
                if (!result.has_value) {
                    result.min = value;
                    result.max = value;
                    result.has_value = true;
                } else {
                    if (value < result.min) {
                        result.min = value;
                    }
                    if (value > result.max) {
                        result.max = value;
                    }
                }
                ++result.count;
            }
            break;
        }
        case FieldType::kFloat64: {
            const auto* values = field.DataFloat64();
            if (!has_nulls) {
                AggregateDouble(values, count, mask, &result);
                break;
            }
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (!field.IsValid(i)) {
                    continue;
                }
                const double value = values[i];
                result.sum += value;
                if (!result.has_value) {
                    result.min = value;
                    result.max = value;
                    result.has_value = true;
                } else {
                    if (value < result.min) {
                        result.min = value;
                    }
                    if (value > result.max) {
                        result.max = value;
                    }
                }
                ++result.count;
            }
            break;
        }
        case FieldType::kBool: {
            const auto* values = field.DataBool();
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                const double value = values[i] ? 1.0 : 0.0;
                result.sum += value;
                if (!result.has_value) {
                    result.min = value;
                    result.max = value;
                    result.has_value = true;
                } else {
                    if (value < result.min) {
                        result.min = value;
                    }
                    if (value > result.max) {
                        result.max = value;
                    }
                }
                ++result.count;
            }
            break;
        }
        case FieldType::kDictInt32: {
            const auto* ids = field.DataDictIds();
            const auto* dict = field.Dictionary();
            if (!ids || !dict) {
                break;
            }
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                const double value = static_cast<double>(dict->Value(ids[i]));
                result.sum += value;
                if (!result.has_value) {
                    result.min = value;
                    result.max = value;
                    result.has_value = true;
                } else {
                    if (value < result.min) {
                        result.min = value;
                    }
                    if (value > result.max) {
                        result.max = value;
                    }
                }
                ++result.count;
            }
            break;
        }
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
        case FieldType::kObject:
            break;
    }
    *out = result;
}

void AggregateCount(const FieldVector& field, const Mask* mask, AggregateResult* out) {
    if (!out) {
        return;
    }
    AggregateResult result;
    const size_t count = field.Size();
    const bool has_nulls = field.HasNulls();
    for (size_t i = 0; i < count; ++i) {
        if (mask && !mask->Get(i)) {
            continue;
        }
        if (has_nulls && !field.IsValid(i)) {
            continue;
        }
        ++result.count;
    }
    *out = result;
}

void AggregateSum(const FieldVector& field, const Mask* mask, AggregateResult* out) {
    if (!out) {
        return;
    }
    AggregateResult result;
    const size_t count = field.Size();
    const bool has_nulls = field.HasNulls();
    switch (field.Type()) {
        case FieldType::kInt32: {
            const auto* values = field.DataInt32();
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                result.sum += static_cast<double>(values[i]);
                ++result.count;
            }
            break;
        }
        case FieldType::kInt64: {
            const auto* values = field.DataInt64();
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                result.sum += static_cast<double>(values[i]);
                ++result.count;
            }
            break;
        }
        case FieldType::kFloat64: {
            const auto* values = field.DataFloat64();
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                result.sum += values[i];
                ++result.count;
            }
            break;
        }
        case FieldType::kBool: {
            const auto* values = field.DataBool();
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                result.sum += values[i] ? 1.0 : 0.0;
                ++result.count;
            }
            break;
        }
        case FieldType::kDictInt32: {
            const auto* ids = field.DataDictIds();
            const auto* dict = field.Dictionary();
            if (!ids || !dict) {
                break;
            }
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                result.sum += static_cast<double>(dict->Value(ids[i]));
                ++result.count;
            }
            break;
        }
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
        case FieldType::kObject:
            break;
    }
    *out = result;
}

void AggregateMinMax(const FieldVector& field, const Mask* mask, AggregateResult* out) {
    if (!out) {
        return;
    }
    AggregateResult result;
    const size_t count = field.Size();
    const bool has_nulls = field.HasNulls();
    switch (field.Type()) {
        case FieldType::kInt32: {
            const auto* values = field.DataInt32();
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                const double value = static_cast<double>(values[i]);
                if (!result.has_value) {
                    result.min = value;
                    result.max = value;
                    result.has_value = true;
                } else {
                    if (value < result.min) {
                        result.min = value;
                    }
                    if (value > result.max) {
                        result.max = value;
                    }
                }
                ++result.count;
            }
            break;
        }
        case FieldType::kInt64: {
            const auto* values = field.DataInt64();
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                const double value = static_cast<double>(values[i]);
                if (!result.has_value) {
                    result.min = value;
                    result.max = value;
                    result.has_value = true;
                } else {
                    if (value < result.min) {
                        result.min = value;
                    }
                    if (value > result.max) {
                        result.max = value;
                    }
                }
                ++result.count;
            }
            break;
        }
        case FieldType::kFloat64: {
            const auto* values = field.DataFloat64();
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                const double value = values[i];
                if (!result.has_value) {
                    result.min = value;
                    result.max = value;
                    result.has_value = true;
                } else {
                    if (value < result.min) {
                        result.min = value;
                    }
                    if (value > result.max) {
                        result.max = value;
                    }
                }
                ++result.count;
            }
            break;
        }
        case FieldType::kBool: {
            const auto* values = field.DataBool();
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                const double value = values[i] ? 1.0 : 0.0;
                if (!result.has_value) {
                    result.min = value;
                    result.max = value;
                    result.has_value = true;
                } else {
                    if (value < result.min) {
                        result.min = value;
                    }
                    if (value > result.max) {
                        result.max = value;
                    }
                }
                ++result.count;
            }
            break;
        }
        case FieldType::kDictInt32: {
            const auto* ids = field.DataDictIds();
            const auto* dict = field.Dictionary();
            if (!ids || !dict) {
                break;
            }
            for (size_t i = 0; i < count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
                    continue;
                }
                const double value = static_cast<double>(dict->Value(ids[i]));
                if (!result.has_value) {
                    result.min = value;
                    result.max = value;
                    result.has_value = true;
                } else {
                    if (value < result.min) {
                        result.min = value;
                    }
                    if (value > result.max) {
                        result.max = value;
                    }
                }
                ++result.count;
            }
            break;
        }
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
        case FieldType::kObject:
            break;
    }
    *out = result;
}

void AggregateMixed(const FieldVector& field, const Mask* mask, AggregateResult* out) {
    AggregateField(field, mask, out);
}

void AggregateSumPredicate(const FieldVector& field, PredicateFn predicate, void* predicate_ctx,
                           AggregateResult* out) {
    if (!out || !predicate) {
        return;
    }
    Mask mask(field.Size());
    BuildMaskLoop(field.Size(), predicate, predicate_ctx, &mask);
    AggregateSum(field, &mask, out);
}

void AggregateCountPredicate(const FieldVector& field, PredicateFn predicate, void* predicate_ctx,
                             AggregateResult* out) {
    if (!out || !predicate) {
        return;
    }
    Mask mask(field.Size());
    BuildMaskLoop(field.Size(), predicate, predicate_ctx, &mask);
    AggregateCount(field, &mask, out);
}

void AggregateMinMaxPredicate(const FieldVector& field, PredicateFn predicate, void* predicate_ctx,
                              AggregateResult* out) {
    if (!out || !predicate) {
        return;
    }
    Mask mask(field.Size());
    BuildMaskLoop(field.Size(), predicate, predicate_ctx, &mask);
    AggregateMinMax(field, &mask, out);
}

void AggregateMixedPredicate(const FieldVector& field, PredicateFn predicate, void* predicate_ctx,
                             AggregateResult* out) {
    if (!out || !predicate) {
        return;
    }
    Mask mask(field.Size());
    BuildMaskLoop(field.Size(), predicate, predicate_ctx, &mask);
    AggregateMixed(field, &mask, out);
}

void AggregateCompressed(const CompressedColumnView& column, const Mask* mask,
                         AggregateResult* out) {
    if (!out || !column.data) {
        return;
    }
    AggregateResult result;
    switch (column.type) {
        case FieldType::kInt32:
        case FieldType::kInt64:
        case FieldType::kFloat64:
        case FieldType::kBool:
        case FieldType::kDictInt32: {
            for (size_t i = 0; i < column.row_count; ++i) {
                if (mask && !mask->Get(i)) {
                    continue;
                }
                double value = 0.0;
                if (!ReadNumericValue(column, i, &value)) {
                    continue;
                }
                result.sum += value;
                if (!result.has_value) {
                    result.min = value;
                    result.max = value;
                    result.has_value = true;
                } else {
                    if (value < result.min) {
                        result.min = value;
                    }
                    if (value > result.max) {
                        result.max = value;
                    }
                }
                ++result.count;
            }
            break;
        }
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
        case FieldType::kObject:
            break;
    }
    *out = result;
}

void AggregateFieldParallel(const FieldVector& field, const Mask* mask, size_t thread_count,
                            AggregateResult* out) {
    if (!out) {
        return;
    }
    const size_t count = field.Size();
    if (thread_count <= 1 || count < thread_count) {
        AggregateField(field, mask, out);
        return;
    }
    const size_t chunk = (count + thread_count - 1) / thread_count;
    std::vector<AggregateResult> partial(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (size_t t = 0; t < thread_count; ++t) {
        const size_t start = t * chunk;
        const size_t end = start + chunk > count ? count : start + chunk;
        threads.emplace_back([&, start, end, t]() {
            if (start >= end) {
                return;
            }
            const bool has_nulls = field.HasNulls();
            switch (field.Type()) {
                case FieldType::kInt32: {
                    const auto* values = field.DataInt32();
                    for (size_t i = start; i < end; ++i) {
                        if (mask && !mask->Get(i)) {
                            continue;
                        }
                        if (has_nulls && !field.IsValid(i)) {
                            continue;
                        }
                        const double value = static_cast<double>(values[i]);
                        partial[t].sum += value;
                        if (!partial[t].has_value) {
                            partial[t].min = value;
                            partial[t].max = value;
                            partial[t].has_value = true;
                        } else {
                            if (value < partial[t].min) {
                                partial[t].min = value;
                            }
                            if (value > partial[t].max) {
                                partial[t].max = value;
                            }
                        }
                        ++partial[t].count;
                    }
                    break;
                }
                case FieldType::kInt64: {
                    const auto* values = field.DataInt64();
                    for (size_t i = start; i < end; ++i) {
                        if (mask && !mask->Get(i)) {
                            continue;
                        }
                        if (has_nulls && !field.IsValid(i)) {
                            continue;
                        }
                        const double value = static_cast<double>(values[i]);
                        partial[t].sum += value;
                        if (!partial[t].has_value) {
                            partial[t].min = value;
                            partial[t].max = value;
                            partial[t].has_value = true;
                        } else {
                            if (value < partial[t].min) {
                                partial[t].min = value;
                            }
                            if (value > partial[t].max) {
                                partial[t].max = value;
                            }
                        }
                        ++partial[t].count;
                    }
                    break;
                }
                case FieldType::kFloat64: {
                    const auto* values = field.DataFloat64();
                    for (size_t i = start; i < end; ++i) {
                        if (mask && !mask->Get(i)) {
                            continue;
                        }
                        if (has_nulls && !field.IsValid(i)) {
                            continue;
                        }
                        const double value = values[i];
                        partial[t].sum += value;
                        if (!partial[t].has_value) {
                            partial[t].min = value;
                            partial[t].max = value;
                            partial[t].has_value = true;
                        } else {
                            if (value < partial[t].min) {
                                partial[t].min = value;
                            }
                            if (value > partial[t].max) {
                                partial[t].max = value;
                            }
                        }
                        ++partial[t].count;
                    }
                    break;
                }
                case FieldType::kBool: {
                    const auto* values = field.DataBool();
                    for (size_t i = start; i < end; ++i) {
                        if (mask && !mask->Get(i)) {
                            continue;
                        }
                        if (has_nulls && !field.IsValid(i)) {
                            continue;
                        }
                        const double value = values[i] ? 1.0 : 0.0;
                        partial[t].sum += value;
                        if (!partial[t].has_value) {
                            partial[t].min = value;
                            partial[t].max = value;
                            partial[t].has_value = true;
                        } else {
                            if (value < partial[t].min) {
                                partial[t].min = value;
                            }
                            if (value > partial[t].max) {
                                partial[t].max = value;
                            }
                        }
                        ++partial[t].count;
                    }
                    break;
                }
                case FieldType::kDictInt32: {
                    const auto* ids = field.DataDictIds();
                    const auto* dict = field.Dictionary();
                    if (!ids || !dict) {
                        break;
                    }
                    for (size_t i = start; i < end; ++i) {
                        if (mask && !mask->Get(i)) {
                            continue;
                        }
                        if (has_nulls && !field.IsValid(i)) {
                            continue;
                        }
                        const double value = static_cast<double>(dict->Value(ids[i]));
                        partial[t].sum += value;
                        if (!partial[t].has_value) {
                            partial[t].min = value;
                            partial[t].max = value;
                            partial[t].has_value = true;
                        } else {
                            if (value < partial[t].min) {
                                partial[t].min = value;
                            }
                            if (value > partial[t].max) {
                                partial[t].max = value;
                            }
                        }
                        ++partial[t].count;
                    }
                    break;
                }
                case FieldType::kString:
                case FieldType::kBytes:
                case FieldType::kArray:
                case FieldType::kObject:
                    break;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    AggregateResult result;
    for (const auto& part : partial) {
        result.count += part.count;
        result.sum += part.sum;
        if (part.has_value) {
            if (!result.has_value) {
                result.min = part.min;
                result.max = part.max;
                result.has_value = true;
            } else {
                if (part.min < result.min) {
                    result.min = part.min;
                }
                if (part.max > result.max) {
                    result.max = part.max;
                }
            }
        }
    }
    *out = result;
}

void AggregateSegmentsParallel(const std::vector<Segment>& segments, size_t field_index,
                               size_t thread_count, AggregateResult* out) {
    if (!out) {
        return;
    }
    if (thread_count <= 1 || segments.size() < thread_count) {
        AggregateResult result;
        for (const auto& segment : segments) {
            if (field_index >= segment.Fields().size()) {
                continue;
            }
            if (segment.IsSealed() && !segment.CompressedColumns().empty()) {
                const auto& columns = segment.CompressedColumns();
                const auto* working = &columns;
                DecodedColumns decoded;
                if (NeedsLz4Decode(columns)) {
                    if (!BuildReadableColumns(columns, &decoded)) {
                        continue;
                    }
                    working = &decoded.views;
                }
                AggregateResult seg_result;
                AggregateCompressed((*working)[field_index], nullptr, &seg_result);
                AccumulateAggregate(&result, seg_result);
            } else {
                AggregateResult seg_result;
                AggregateField(segment.Fields()[field_index], nullptr, &seg_result);
                AccumulateAggregate(&result, seg_result);
            }
        }
        *out = result;
        return;
    }
    const auto schedule = ScheduleSegments(segments.size(), thread_count);
    std::vector<AggregateResult> partial(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t]() {
            DecodedColumns decoded;
            for (size_t idx : schedule[t]) {
                const auto& segment = segments[idx];
                if (field_index >= segment.Fields().size()) {
                    continue;
                }
                if (segment.IsSealed() && !segment.CompressedColumns().empty()) {
                    const auto& columns = segment.CompressedColumns();
                    const auto* working = &columns;
                    if (NeedsLz4Decode(columns)) {
                        if (!BuildReadableColumns(columns, &decoded)) {
                            continue;
                        }
                        working = &decoded.views;
                    }
                    AggregateResult seg_result;
                    AggregateCompressed((*working)[field_index], nullptr, &seg_result);
                    AccumulateAggregate(&partial[t], seg_result);
                } else {
                    AggregateResult seg_result;
                    AggregateField(segment.Fields()[field_index], nullptr, &seg_result);
                    AccumulateAggregate(&partial[t], seg_result);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    AggregateResult result;
    for (size_t t = 0; t < partial.size(); ++t) {
        AccumulateAggregate(&result, partial[t]);
    }
    *out = result;
}

}  // namespace mimicdb
