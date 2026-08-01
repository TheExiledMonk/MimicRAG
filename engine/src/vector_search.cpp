#include "mimicdb/vector_search.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <future>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include "mimicdb/dataset.h"
#include "mimicdb/compression.h"

namespace mimicdb {
namespace {
float Dot(const float* a, const float* b, size_t n) {
    float sum = 0.0F;
    for (size_t i = 0; i < n; ++i) sum += a[i] * b[i];
    return sum;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,fma")))
float DotAvx2(const float* a, const float* b, size_t n) {
    __m256 sum = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        sum = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), sum);
    }
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, sum);
    float result = lanes[0] + lanes[1] + lanes[2] + lanes[3] +
                   lanes[4] + lanes[5] + lanes[6] + lanes[7];
    for (; i < n; ++i) result += a[i] * b[i];
    return result;
}

bool HasAvx2() {
    static const bool available = [] { __builtin_cpu_init(); return __builtin_cpu_supports("avx2") &&
        __builtin_cpu_supports("fma"); }();
    return available;
}
#endif

float FastDot(const float* a, const float* b, size_t n) {
#if defined(__x86_64__) || defined(__i386__)
    if (HasAvx2()) return DotAvx2(a, b, n);
#endif
    return Dot(a, b, n);
}

struct Worse {
    bool operator()(const VectorSearchHit& a, const VectorSearchHit& b) const {
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.row_id < b.row_id;
    }
};

bool Compare(double left, CompareOp op, double right) {
    switch (op) {
        case CompareOp::kEq: return left == right;
        case CompareOp::kNe: return left != right;
        case CompareOp::kLt: return left < right;
        case CompareOp::kLe: return left <= right;
        case CompareOp::kGt: return left > right;
        case CompareOp::kGe: return left >= right;
    }
    return false;
}

bool ReadNumeric(const FieldVector& field, size_t row, double* out) {
    if (!field.IsValid(row)) return false;
    switch (field.Type()) {
        case FieldType::kInt32: *out = field.DataInt32()[row]; return true;
        case FieldType::kInt64: *out = static_cast<double>(field.DataInt64()[row]); return true;
        case FieldType::kFloat64: *out = field.DataFloat64()[row]; return true;
        case FieldType::kBool: *out = field.DataBool()[row] ? 1.0 : 0.0; return true;
        case FieldType::kDictInt32: *out = field.DictionaryValue(field.DataDictIds()[row]); return true;
        default: return false;
    }
}

bool Matches(const std::vector<FieldVector>& fields, size_t row,
             const std::vector<CompressedColumnView>* compressed,
             const std::vector<VectorSearchPredicate>& predicates) {
    for (const auto& predicate : predicates) {
        if (predicate.field_index >= fields.size()) return false;
        double value = 0.0;
        const bool ok = compressed && predicate.field_index < compressed->size()
            ? ReadNumericValue((*compressed)[predicate.field_index], row, &value)
            : ReadNumeric(fields[predicate.field_index], row, &value);
        if (!ok ||
            !Compare(value, predicate.op, predicate.value)) return false;
    }
    return true;
}

void SearchField(const std::vector<FieldVector>& fields, size_t vector_field,
                 uint64_t base_row, const float* query,
                 size_t dimension, size_t top_k, VectorMetric metric,
                 const std::vector<CompressedColumnView>* compressed,
                 const std::vector<VectorSearchPredicate>& predicates,
                 std::priority_queue<VectorSearchHit, std::vector<VectorSearchHit>, Worse>* heap) {
    const auto& field = fields[vector_field];
    for (size_t row = 0; row < field.Size(); ++row) {
        if (!predicates.empty() && !Matches(fields, row, compressed, predicates)) continue;
        size_t stored_dimension = 0;
        const float* vector = field.VectorFloat32(row, &stored_dimension);
        if (!vector || stored_dimension != dimension) continue;
        VectorSearchHit hit{base_row + row, VectorDistance(vector, query, dimension, metric)};
        if (!std::isfinite(hit.distance)) continue;
        if (heap->size() < top_k) heap->push(hit);
        else if (hit.distance < heap->top().distance ||
                 (hit.distance == heap->top().distance && hit.row_id < heap->top().row_id)) {
            heap->pop();
            heap->push(hit);
        }
    }
}
}  // namespace

float VectorDistance(const float* left, const float* right, size_t dimension,
                     VectorMetric metric) {
    if (metric == VectorMetric::kDot) return -FastDot(left, right, dimension);
    if (metric == VectorMetric::kCosine) {
        const float dot = FastDot(left, right, dimension);
        const float ln = FastDot(left, left, dimension);
        const float rn = FastDot(right, right, dimension);
        if (ln <= 0.0F || rn <= 0.0F) return 1.0F;
        return 1.0F - dot / std::sqrt(ln * rn);
    }
    float sum = 0.0F;
    for (size_t i = 0; i < dimension; ++i) {
        const float delta = left[i] - right[i];
        sum += delta * delta;
    }
    return sum;
}

bool VectorSearch(const Dataset& dataset, size_t field_index, const float* query,
                  size_t dimension, size_t top_k, VectorMetric metric,
                  std::vector<VectorSearchHit>* out,
                  const std::vector<VectorSearchPredicate>& predicates) {
    if (!out || !query || dimension == 0 || top_k == 0 ||
        field_index >= dataset.Fields().size() ||
        dataset.Fields()[field_index].Type() != FieldType::kVectorFloat32 ||
        (dataset.VectorDimension(field_index) != 0 &&
         dataset.VectorDimension(field_index) != dimension)) return false;
    for (size_t i = 0; i < dimension; ++i) if (!std::isfinite(query[i])) return false;
    for (const auto& predicate : predicates) {
        if (predicate.field_index >= dataset.Fields().size()) return false;
    }
    struct Work { const std::vector<FieldVector>* fields; const std::vector<CompressedColumnView>* compressed; uint64_t base; };
    std::vector<Work> work;
    uint64_t base = 0;
    for (const auto& segment : dataset.Segments()) {
        if (field_index >= segment.Fields().size()) return false;
        work.push_back({&segment.Fields(), &segment.CompressedColumns(), base});
        base += segment.RowCount();
    }
    if (dataset.ActiveRowCount() != 0) work.push_back({&dataset.ActiveFields(), nullptr, base});
    auto search_one = [&](Work item) {
        std::priority_queue<VectorSearchHit, std::vector<VectorSearchHit>, Worse> local;
        SearchField(*item.fields, field_index, item.base, query, dimension, top_k, metric,
                    item.compressed,
                    predicates, &local);
        std::vector<VectorSearchHit> hits;
        while (!local.empty()) { hits.push_back(local.top()); local.pop(); }
        return hits;
    };
    std::vector<std::future<std::vector<VectorSearchHit>>> futures;
    futures.reserve(work.size());
    for (const auto item : work) futures.push_back(std::async(std::launch::async, search_one, item));
    std::priority_queue<VectorSearchHit, std::vector<VectorSearchHit>, Worse> heap;
    for (auto& future : futures) {
        for (const auto& hit : future.get()) {
            if (heap.size() < top_k) heap.push(hit);
            else if (hit.distance < heap.top().distance ||
                     (hit.distance == heap.top().distance && hit.row_id < heap.top().row_id)) {
                heap.pop(); heap.push(hit);
            }
        }
    }
    out->resize(heap.size());
    for (size_t i = heap.size(); i > 0; --i) { (*out)[i - 1] = heap.top(); heap.pop(); }
    std::sort(out->begin(), out->end(), [](const auto& a, const auto& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.row_id < b.row_id;
    });
    return true;
}

}  // namespace mimicdb
