#ifndef PCDB_SCAN_H
#define PCDB_SCAN_H

#include <cstddef>
#include <vector>

#include "pcdb/mask.h"
#include "pcdb/metrics.h"
#include "pcdb/segment.h"

namespace pcdb {

using PredicateFn = bool (*)(size_t index, void* ctx);
using ConsumeFn = void (*)(size_t index, void* ctx);
using ScanLoopFn = void (*)(size_t count, PredicateFn predicate, void* predicate_ctx,
                            ConsumeFn consume, void* consume_ctx);
using ScanLoopMaskedFn = void (*)(size_t count, const Mask& mask, ConsumeFn consume,
                                  void* consume_ctx);

void ScanLoop(size_t count, PredicateFn predicate, void* predicate_ctx, ConsumeFn consume,
              void* consume_ctx);
void ScanLoopMasked(size_t count, const Mask& mask, ConsumeFn consume, void* consume_ctx);
void ScanLoopWithMetrics(size_t count, PredicateFn predicate, void* predicate_ctx, ConsumeFn consume,
                          void* consume_ctx, Metrics* metrics, size_t row_bytes);
void ScanLoopMaskedWithMetrics(size_t count, const Mask& mask, ConsumeFn consume, void* consume_ctx,
                               Metrics* metrics, size_t row_bytes);
void BuildMaskLoop(size_t count, PredicateFn predicate, void* predicate_ctx, Mask* out);
void ScanLoopMaskedWithValidity(size_t count, const Mask& mask, const uint8_t* validity,
                                ConsumeFn consume, void* consume_ctx);
bool SegmentMatchesPredicate(const SegmentColumnStats& stats, CompareOp op, double value);
void CollectRowIds(const Mask& mask, size_t count, std::vector<size_t>* out);
std::vector<std::vector<size_t>> ScheduleSegments(size_t segment_count, size_t thread_count);
void ScanSegmentsParallel(const std::vector<Segment>& segments, size_t thread_count,
                          PredicateFn predicate, void* predicate_ctx, ConsumeFn consume,
                          const std::vector<void*>& consume_ctxs);
void CollectRowIdsPacked(const PackedMask& mask, size_t count, std::vector<size_t>* out);
std::vector<size_t> PruneSegmentsByPredicate(const std::vector<Segment>& segments,
                                             size_t field_index, CompareOp op, double value);
std::vector<size_t> PruneSegmentsByPredicateWithMetrics(const std::vector<Segment>& segments,
                                                        size_t field_index, CompareOp op,
                                                        double value, Metrics* metrics);
ScanLoopFn GetScanKernel();
ScanLoopMaskedFn GetScanKernelMasked();

}  // namespace pcdb

#endif  // PCDB_SCAN_H
