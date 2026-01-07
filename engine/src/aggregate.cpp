#include "pcdb/aggregate.h"

#include <cstddef>
#include <thread>
#include <vector>

namespace pcdb {

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
    AggregateResult result;
    const size_t count = field.Size();
    const bool has_nulls = field.HasNulls();
    switch (field.Type()) {
        case FieldType::kInt32: {
            const auto* values = field.DataInt32();
            for (size_t i = 0; i < count; ++i) {
                if (!predicate(i, predicate_ctx)) {
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
                if (!predicate(i, predicate_ctx)) {
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
                if (!predicate(i, predicate_ctx)) {
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
                if (!predicate(i, predicate_ctx)) {
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
                if (!predicate(i, predicate_ctx)) {
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
    }
    *out = result;
}

void AggregateCountPredicate(const FieldVector& field, PredicateFn predicate, void* predicate_ctx,
                             AggregateResult* out) {
    if (!out || !predicate) {
        return;
    }
    AggregateResult result;
    const size_t count = field.Size();
    const bool has_nulls = field.HasNulls();
    for (size_t i = 0; i < count; ++i) {
        if (!predicate(i, predicate_ctx)) {
            continue;
        }
        if (has_nulls && !field.IsValid(i)) {
            continue;
        }
        ++result.count;
    }
    *out = result;
}

void AggregateMinMaxPredicate(const FieldVector& field, PredicateFn predicate, void* predicate_ctx,
                              AggregateResult* out) {
    if (!out || !predicate) {
        return;
    }
    AggregateResult result;
    const size_t count = field.Size();
    const bool has_nulls = field.HasNulls();
    switch (field.Type()) {
        case FieldType::kInt32: {
            const auto* values = field.DataInt32();
            for (size_t i = 0; i < count; ++i) {
                if (!predicate(i, predicate_ctx)) {
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
                if (!predicate(i, predicate_ctx)) {
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
                if (!predicate(i, predicate_ctx)) {
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
                if (!predicate(i, predicate_ctx)) {
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
                if (!predicate(i, predicate_ctx)) {
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
    }
    *out = result;
}

void AggregateMixedPredicate(const FieldVector& field, PredicateFn predicate, void* predicate_ctx,
                             AggregateResult* out) {
    if (!out || !predicate) {
        return;
    }
    AggregateResult result;
    const size_t count = field.Size();
    const bool has_nulls = field.HasNulls();
    switch (field.Type()) {
        case FieldType::kInt32: {
            const auto* values = field.DataInt32();
            for (size_t i = 0; i < count; ++i) {
                if (!predicate(i, predicate_ctx)) {
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
                if (!predicate(i, predicate_ctx)) {
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
            for (size_t i = 0; i < count; ++i) {
                if (!predicate(i, predicate_ctx)) {
                    continue;
                }
                if (has_nulls && !field.IsValid(i)) {
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
                if (!predicate(i, predicate_ctx)) {
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
                if (!predicate(i, predicate_ctx)) {
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

}  // namespace pcdb
