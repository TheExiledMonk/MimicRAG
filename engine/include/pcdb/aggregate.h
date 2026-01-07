#ifndef PCDB_AGGREGATE_H
#define PCDB_AGGREGATE_H

#include <cstdint>

#include "pcdb/field_vector.h"
#include "pcdb/mask.h"
#include "pcdb/scan.h"

namespace pcdb {

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

}  // namespace pcdb

#endif  // PCDB_AGGREGATE_H
