#ifndef PCDB_SIMD_H
#define PCDB_SIMD_H

#include <cstddef>
#include <cstdint>

#include "pcdb/aggregate.h"
#include "pcdb/field_vector.h"
#include "pcdb/mask.h"

namespace pcdb {

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

}  // namespace pcdb

#endif  // PCDB_SIMD_H
