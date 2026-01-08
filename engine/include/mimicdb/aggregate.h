#ifndef MIMICDB_AGGREGATE_H
#define MIMICDB_AGGREGATE_H

#include <cstdint>

#include "mimicdb/compression.h"
#include "mimicdb/field_vector.h"
#include "mimicdb/mask.h"
#include "mimicdb/scan.h"

namespace mimicdb {

struct AggregateResult {
    uint64_t count = 0;
    double sum = 0.0;
    double min = 0.0;
    double max = 0.0;
    bool has_value = false;
};

void AggregateDouble(const double* values, size_t count, const Mask* mask,
                     AggregateResult* out);
void AggregateField(const FieldVector& field, const Mask* mask, AggregateResult* out);
void AggregateFieldParallel(const FieldVector& field, const Mask* mask, size_t thread_count,
                            AggregateResult* out);
void AggregateCount(const FieldVector& field, const Mask* mask, AggregateResult* out);
void AggregateSum(const FieldVector& field, const Mask* mask, AggregateResult* out);
void AggregateMinMax(const FieldVector& field, const Mask* mask, AggregateResult* out);
void AggregateMixed(const FieldVector& field, const Mask* mask, AggregateResult* out);
void AggregateSumPredicate(const FieldVector& field, PredicateFn predicate, void* predicate_ctx,
                           AggregateResult* out);
void AggregateCountPredicate(const FieldVector& field, PredicateFn predicate, void* predicate_ctx,
                             AggregateResult* out);
void AggregateMinMaxPredicate(const FieldVector& field, PredicateFn predicate, void* predicate_ctx,
                              AggregateResult* out);
void AggregateMixedPredicate(const FieldVector& field, PredicateFn predicate, void* predicate_ctx,
                             AggregateResult* out);
void AggregateCompressed(const CompressedColumnView& column, const Mask* mask,
                         AggregateResult* out);

}  // namespace mimicdb

#endif  // MIMICDB_AGGREGATE_H
