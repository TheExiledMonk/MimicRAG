#include "mimicdb/simd.h"

#include "mimicdb/aggregate.h"

namespace mimicdb {

namespace {
void AggregateDoubleScalar(const double* values, size_t count, const Mask* mask,
                           AggregateResult* out) {
    AggregateDouble(values, count, mask, out);
}
}  // namespace

AggregateKernelDouble GetAggregateKernelDouble() {
    return &AggregateDoubleScalar;
}

void AggregateCountScalar(const FieldVector& field, const Mask* mask, AggregateResult* out) {
    AggregateCount(field, mask, out);
}

void AggregateSumScalar(const FieldVector& field, const Mask* mask, AggregateResult* out) {
    AggregateSum(field, mask, out);
}

void AggregateMinMaxScalar(const FieldVector& field, const Mask* mask, AggregateResult* out) {
    AggregateMinMax(field, mask, out);
}

void AggregateMixedScalar(const FieldVector& field, const Mask* mask, AggregateResult* out) {
    AggregateMixed(field, mask, out);
}

AggregateKernel GetAggregateKernelCount() {
    return &AggregateCountScalar;
}

AggregateKernel GetAggregateKernelSum() {
    return &AggregateSumScalar;
}

AggregateKernel GetAggregateKernelMinMax() {
    return &AggregateMinMaxScalar;
}

AggregateKernel GetAggregateKernelMixed() {
    return &AggregateMixedScalar;
}

}  // namespace mimicdb
