#ifndef MIMICDB_SIMD_H
#define MIMICDB_SIMD_H

#include <cstddef>
#include <cstdint>

#include "mimicdb/aggregate.h"
#include "mimicdb/field_vector.h"
#include "mimicdb/mask.h"

namespace mimicdb {

using PredicateKernelInt64 = void (*)(const int64_t* data, int64_t value, uint8_t* out,
                                      size_t count);
using AggregateKernelDouble = void (*)(const double* values, size_t count, const Mask* mask,
                                       AggregateResult* out);
using AggregateKernel = void (*)(const FieldVector& field, const Mask* mask, AggregateResult* out);

PredicateKernelInt64 GetPredicateKernelInt64Eq();
AggregateKernelDouble GetAggregateKernelDouble();
AggregateKernel GetAggregateKernelCount();
AggregateKernel GetAggregateKernelSum();
AggregateKernel GetAggregateKernelMinMax();
AggregateKernel GetAggregateKernelMixed();

bool CpuHasAvx2();
bool CpuHasNeon();

}  // namespace mimicdb

#endif  // MIMICDB_SIMD_H
