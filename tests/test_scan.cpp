#include <cassert>
#include <cstddef>

#include "mimicdb/mask.h"
#include "mimicdb/scan.h"
#include "mimicdb/output_compaction.h"
#include "mimicdb/simd_output.h"

namespace {
struct PredCtx {
    const int* data;
    int threshold;
};

bool Predicate(size_t index, void* ctx) {
    const auto* pred = static_cast<PredCtx*>(ctx);
    return pred->data[index] > pred->threshold;
}

struct CountCtx {
    size_t count = 0;
};

void Consume(size_t /*index*/, void* ctx) {
    auto* count = static_cast<CountCtx*>(ctx);
    ++count->count;
}
}  // namespace

int main() {
    const int data[] = {1, 3, 5, 2, 4};
    PredCtx pred{data, 3};
    CountCtx out;

    mimicdb::ScanLoop(5, Predicate, &pred, Consume, &out);
    assert(out.count == 2);

    mimicdb::Mask mask(5);
    mask.Set(1, true);
    mask.Set(4, true);
    CountCtx masked;
    mimicdb::ScanLoopMasked(5, mask, Consume, &masked);
    assert(masked.count == 2);

    const uint8_t validity[] = {1, 0, 1, 1, 1};
    CountCtx masked_valid;
    mimicdb::ScanLoopMaskedWithValidity(5, mask, validity, Consume, &masked_valid);
    assert(masked_valid.count == 1);

    std::vector<size_t> row_ids;
    mimicdb::CollectRowIds(mask, 5, &row_ids);
    assert(row_ids.size() == 2);
    assert(row_ids[0] == 1);
    assert(row_ids[1] == 4);

    std::vector<size_t> compacted;
    mimicdb::CompactRowIdsPrefixSum(mask, 5, &compacted);
    assert(compacted.size() == 2);
    assert(compacted[0] == 1);
    assert(compacted[1] == 4);

    std::vector<size_t> simd_out;
    mimicdb::CompressStoreScalar(mask, 5, &simd_out);
    assert(simd_out.size() == 2);
    assert(simd_out[0] == 1);
    assert(simd_out[1] == 4);

    mimicdb::PackedMask packed = mimicdb::PackMask(mask);
    std::vector<size_t> packed_ids;
    mimicdb::CollectRowIdsPacked(packed, 5, &packed_ids);
    assert(packed_ids.size() == 2);
    assert(packed_ids[0] == 1);
    assert(packed_ids[1] == 4);

    std::vector<mimicdb::Segment> segments;
    segments.emplace_back(4, 2, std::vector<mimicdb::FieldVector>{});
    segments.emplace_back(4, 3, std::vector<mimicdb::FieldVector>{});
    segments.emplace_back(4, 1, std::vector<mimicdb::FieldVector>{});

    struct PredAlwaysCtx {};
    auto pred_always = [](size_t /*index*/, void* /*ctx*/) { return true; };
    CountCtx thread_counts[2];
    std::vector<void*> consume_ctxs = {&thread_counts[0], &thread_counts[1]};
    mimicdb::ScanSegmentsParallel(segments, 2, pred_always, nullptr, Consume, consume_ctxs);
    assert(thread_counts[0].count + thread_counts[1].count == 6);

    return 0;
}
