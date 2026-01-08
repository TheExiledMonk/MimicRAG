#include "mimicdb/scan.h"

#include <thread>

#include "mimicdb/predicate.h"

namespace mimicdb {

void ScanLoop(size_t count, PredicateFn predicate, void* predicate_ctx, ConsumeFn consume,
              void* consume_ctx) {
    if (!predicate || !consume) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (predicate(i, predicate_ctx)) {
            consume(i, consume_ctx);
        }
    }
}

void ScanLoopMasked(size_t count, const Mask& mask, ConsumeFn consume, void* consume_ctx) {
    if (!consume) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (mask.Get(i)) {
            consume(i, consume_ctx);
        }
    }
}

void ScanLoopWithMetrics(size_t count, PredicateFn predicate, void* predicate_ctx, ConsumeFn consume,
                         void* consume_ctx, Metrics* metrics, size_t row_bytes) {
    if (metrics) {
        metrics->AddRows(static_cast<uint64_t>(count));
        metrics->AddBytes(static_cast<uint64_t>(count * row_bytes));
    }
    ScanLoop(count, predicate, predicate_ctx, consume, consume_ctx);
}

void ScanLoopMaskedWithMetrics(size_t count, const Mask& mask, ConsumeFn consume, void* consume_ctx,
                               Metrics* metrics, size_t row_bytes) {
    if (metrics) {
        metrics->AddRows(static_cast<uint64_t>(count));
        metrics->AddBytes(static_cast<uint64_t>(count * row_bytes));
    }
    ScanLoopMasked(count, mask, consume, consume_ctx);
}

void BuildMaskLoop(size_t count, PredicateFn predicate, void* predicate_ctx, Mask* out) {
    if (!out || !predicate) {
        return;
    }
    out->Resize(count);
    for (size_t i = 0; i < count; ++i) {
        out->Set(i, predicate(i, predicate_ctx));
    }
}

void ScanLoopMaskedWithValidity(size_t count, const Mask& mask, const uint8_t* validity,
                                ConsumeFn consume, void* consume_ctx) {
    if (!consume) {
        return;
    }
    if (!validity) {
        ScanLoopMasked(count, mask, consume, consume_ctx);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (mask.Get(i) && validity[i]) {
            consume(i, consume_ctx);
        }
    }
}

bool SegmentMatchesPredicate(const SegmentColumnStats& stats, CompareOp op, double value) {
    if (!stats.has_value) {
        return true;
    }
    return PredicateCanMatchRange(stats.min, stats.max, op, value);
}

void CollectRowIds(const Mask& mask, size_t count, std::vector<size_t>* out) {
    if (!out) {
        return;
    }
    out->clear();
    for (size_t i = 0; i < count; ++i) {
        if (mask.Get(i)) {
            out->push_back(i);
        }
    }
}

void CollectRowIdsPacked(const PackedMask& mask, size_t count, std::vector<size_t>* out) {
    if (!out) {
        return;
    }
    out->clear();
    for (size_t i = 0; i < count; ++i) {
        if (mask.Get(i)) {
            out->push_back(i);
        }
    }
}

std::vector<std::vector<size_t>> ScheduleSegments(size_t segment_count, size_t thread_count) {
    std::vector<std::vector<size_t>> schedule;
    if (thread_count == 0) {
        return schedule;
    }
    schedule.resize(thread_count);
    for (size_t i = 0; i < segment_count; ++i) {
        schedule[i % thread_count].push_back(i);
    }
    return schedule;
}

void ScanSegmentsParallel(const std::vector<Segment>& segments, size_t thread_count,
                          PredicateFn predicate, void* predicate_ctx, ConsumeFn consume,
                          const std::vector<void*>& consume_ctxs) {
    if (thread_count == 0 || segments.empty() || !consume || !predicate) {
        return;
    }
    if (consume_ctxs.size() < thread_count) {
        return;
    }
    const auto schedule = ScheduleSegments(segments.size(), thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t]() {
            for (size_t idx : schedule[t]) {
                ScanLoop(segments[idx].RowCount(), predicate, predicate_ctx, consume, consume_ctxs[t]);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
}

std::vector<size_t> PruneSegmentsByPredicate(const std::vector<Segment>& segments,
                                             size_t field_index, CompareOp op, double value) {
    std::vector<size_t> indices;
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& stats = segments[i].ColumnStats();
        if (field_index >= stats.size()) {
            continue;
        }
        if (SegmentMatchesPredicate(stats[field_index], op, value)) {
            indices.push_back(i);
        }
    }
    return indices;
}

std::vector<size_t> PruneSegmentsByPredicateWithMetrics(const std::vector<Segment>& segments,
                                                        size_t field_index, CompareOp op,
                                                        double value, Metrics* metrics) {
    if (metrics) {
        metrics->AddSegmentsTotal(static_cast<uint64_t>(segments.size()));
    }
    std::vector<size_t> indices;
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& stats = segments[i].ColumnStats();
        if (field_index >= stats.size()) {
            if (metrics) {
                metrics->AddSegmentsPruned(1);
            }
            continue;
        }
        if (SegmentMatchesPredicate(stats[field_index], op, value)) {
            indices.push_back(i);
            if (metrics) {
                metrics->AddSegmentsScanned(1);
            }
        } else if (metrics) {
            metrics->AddSegmentsPruned(1);
        }
    }
    return indices;
}

}  // namespace mimicdb
