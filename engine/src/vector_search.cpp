#include "mimicdb/vector_search.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <queue>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include "mimicdb/dataset.h"
#include "mimicdb/compression.h"
#include "mimicdb/vector_gpu.h"

namespace mimicdb {
bool VectorGpuScore(const Dataset&, size_t, const float*, size_t, VectorMetric,
                    const std::vector<uint32_t>&, std::vector<float>*);
}

namespace mimicdb {
namespace {
struct AutoRoutingState {
    std::mutex mutex;
    size_t crossover_elements = 500000000ULL;
    size_t max_tested_elements = 0;
    bool calibrated = false;
};

AutoRoutingState& RoutingState() {
    static AutoRoutingState state;
    return state;
}

thread_local int route_override = -1;  // -1 automatic, 0 CPU, 1 Vulkan
std::atomic<size_t> configured_cpu_threads{0};

size_t CpuThreadCount() {
    size_t threads = std::max(1U, std::thread::hardware_concurrency());
    const size_t configured = configured_cpu_threads.load(std::memory_order_relaxed);
    if (configured != 0) threads = configured;
    if (const char* value = std::getenv("MIMICDB_VECTOR_CPU_THREADS")) {
        const unsigned long long parsed = std::strtoull(value, nullptr, 10);
        if (parsed != 0) threads = static_cast<size_t>(parsed);
    }
    return threads;
}

class VectorWorkerPool {
public:
    static VectorWorkerPool& Instance() {
        static VectorWorkerPool pool(CpuThreadCount());
        return pool;
    }

    ~VectorWorkerPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_) worker.join();
    }

    size_t Size() const { return workers_.size(); }

    template <typename Function>
    void Run(size_t count, Function function) {
        if (count == 0) return;
        const size_t task_count = std::min(count, workers_.size());
        if (task_count <= 1 || is_worker_) {
            for (size_t i = 0; i < count; ++i) function(i);
            return;
        }
        struct Batch {
            std::atomic<size_t> next{0};
            std::atomic<size_t> remaining{0};
            std::mutex mutex;
            std::condition_variable done;
            std::exception_ptr error;
        };
        auto batch = std::make_shared<Batch>();
        batch->remaining = task_count;
        auto shared_function = std::make_shared<Function>(std::move(function));
        for (size_t task = 0; task < task_count; ++task) {
            Enqueue([batch, shared_function, count] {
                try {
                    for (;;) {
                        const size_t index = batch->next.fetch_add(1, std::memory_order_relaxed);
                        if (index >= count) break;
                        (*shared_function)(index);
                    }
                } catch (...) {
                    std::lock_guard lock(batch->mutex);
                    if (!batch->error) batch->error = std::current_exception();
                }
                if (batch->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    batch->done.notify_one();
            });
        }
        std::unique_lock lock(batch->mutex);
        batch->done.wait(lock, [&] { return batch->remaining.load(std::memory_order_acquire) == 0; });
        if (batch->error) std::rethrow_exception(batch->error);
    }

private:
    explicit VectorWorkerPool(size_t count) {
        workers_.reserve(std::max<size_t>(1, count));
        for (size_t i = 0; i < std::max<size_t>(1, count); ++i) {
            workers_.emplace_back([this] {
                is_worker_ = true;
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock(mutex_);
                        ready_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
                        if (stopping_ && tasks_.empty()) break;
                        task = std::move(tasks_.front());
                        tasks_.pop_front();
                    }
                    task();
                }
                is_worker_ = false;
            });
        }
    }

    void Enqueue(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        ready_.notify_one();
    }

    static thread_local bool is_worker_;
    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool stopping_ = false;
};

thread_local bool VectorWorkerPool::is_worker_ = false;

template <typename Function>
void ParallelFor(size_t count, Function function) {
    VectorWorkerPool::Instance().Run(count, std::move(function));
}

bool IsAutoMode() {
    const char* value = std::getenv("MIMICDB_VECTOR_BACKEND");
    return !value || std::string(value) == "auto";
}

bool WantsGpu(size_t candidates, size_t dimension) {
    if (route_override == 0) return false;
    if (route_override == 1) return true;
    const char* mode_value = std::getenv("MIMICDB_VECTOR_BACKEND");
    const std::string mode = mode_value ? mode_value : "auto";
    if (mode == "cpu" || mode == "off") return false;
    if (mode == "vulkan" || mode == "gpu") return true;
    size_t threshold = 0;
    if (const char* value = std::getenv("MIMICDB_VECTOR_GPU_MIN_ELEMENTS")) {
        const unsigned long long parsed = std::strtoull(value, nullptr, 10);
        if (parsed != 0) threshold = static_cast<size_t>(parsed);
    }
    if (threshold == 0) {
        std::lock_guard lock(RoutingState().mutex);
        threshold = RoutingState().crossover_elements;
    }
    return candidates != 0 && dimension <= SIZE_MAX / candidates && candidates * dimension >= threshold;
}
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

float PreparedDistance(const float* left, const float* right, size_t dimension,
                       VectorMetric metric, float query_norm_squared) {
    if (metric == VectorMetric::kDot) return -FastDot(left, right, dimension);
    if (metric == VectorMetric::kCosine) {
        const float dot = FastDot(left, right, dimension);
        const float left_norm_squared = FastDot(left, left, dimension);
        if (left_norm_squared <= 0.0F || query_norm_squared <= 0.0F) return 1.0F;
        return 1.0F - dot / std::sqrt(left_norm_squared * query_norm_squared);
    }
    float sum = 0.0F;
    for (size_t i = 0; i < dimension; ++i) {
        const float delta = left[i] - right[i];
        sum += delta * delta;
    }
    return sum;
}

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
                 float query_norm_squared,
                 const std::vector<CompressedColumnView>* compressed,
                 const std::vector<VectorSearchPredicate>& predicates,
                 const std::vector<size_t>* precomputed_candidates,
                 std::priority_queue<VectorSearchHit, std::vector<VectorSearchHit>, Worse>* heap) {
    const auto& field = fields[vector_field];
    auto score_row = [&](size_t row) {
        size_t stored_dimension = 0;
        const float* vector = field.VectorFloat32(row, &stored_dimension);
        if (!vector || stored_dimension != dimension) return;
        VectorSearchHit hit{base_row + row,
                            PreparedDistance(vector, query, dimension, metric,
                                             query_norm_squared)};
        if (!std::isfinite(hit.distance)) return;
        if (heap->size() < top_k) heap->push(hit);
        else if (hit.distance < heap->top().distance ||
                 (hit.distance == heap->top().distance && hit.row_id < heap->top().row_id)) {
            heap->pop();
            heap->push(hit);
        }
    };

    if (precomputed_candidates) {
        for (const size_t row : *precomputed_candidates) score_row(row);
        return;
    }

    if (predicates.empty()) {
        for (size_t row = 0; row < field.Size(); ++row) score_row(row);
        return;
    }

    // Resolve structured predicates completely before touching vector storage. Besides
    // keeping rejected vectors out of cache, the candidate list gives selective queries
    // a sparse iteration path with work proportional to the number of matching rows.
    std::vector<size_t> candidates;
    candidates.reserve(std::min<size_t>(field.Size(), 1024));
    for (size_t row = 0; row < field.Size(); ++row) {
        if (Matches(fields, row, compressed, predicates)) {
            candidates.push_back(row);
        }
    }
    for (const size_t row : candidates) score_row(row);
}
}  // namespace

float VectorDistance(const float* left, const float* right, size_t dimension,
                     VectorMetric metric) {
    const float query_norm_squared = metric == VectorMetric::kCosine
        ? FastDot(right, right, dimension) : 0.0F;
    return PreparedDistance(left, right, dimension, metric, query_norm_squared);
}

namespace {
void CalibrateGpuCrossover(const Dataset& dataset, size_t field_index,
                           const float* query, size_t dimension, VectorMetric metric,
                           size_t top_k, size_t sealed_rows) {
    if (route_override != -1 || !IsAutoMode() || sealed_rows < 16384 || dimension == 0) return;
    const size_t elements = sealed_rows > SIZE_MAX / dimension
        ? SIZE_MAX : sealed_rows * dimension;
    auto& state = RoutingState();
    std::unique_lock state_lock(state.mutex);
    if (state.calibrated && elements <= state.max_tested_elements * 2) return;

    if (!PreloadVectorField(dataset, field_index)) return;
    std::vector<VectorSearchHit> calibration_hits;
    route_override = 0;
    const auto cpu_start = std::chrono::steady_clock::now();
    const bool cpu_ok = VectorSearch(dataset, field_index, query, dimension, top_k,
                                     metric, &calibration_hits);
    const double cpu_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - cpu_start).count();

    route_override = 1;
    const auto gpu_start = std::chrono::steady_clock::now();
    const bool gpu_ok = VectorSearch(dataset, field_index, query, dimension, top_k,
                                     metric, &calibration_hits);
    const double gpu_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - gpu_start).count();
    route_override = -1;
    const size_t tested = sealed_rows * dimension;
    const bool gpu_wins = cpu_ok && gpu_ok && gpu_seconds < cpu_seconds * 0.90;

    state.calibrated = true;
    state.max_tested_elements = std::max(state.max_tested_elements, tested);
    if (gpu_wins)
        state.crossover_elements = tested;
    else
        state.crossover_elements = tested > SIZE_MAX / 2 ? SIZE_MAX : tested * 2;
}
}  // namespace

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
    const float query_norm_squared = metric == VectorMetric::kCosine
        ? FastDot(query, query, dimension) : 0.0F;
    for (const auto& predicate : predicates) {
        if (predicate.field_index >= dataset.Fields().size()) return false;
    }
    size_t sealed_rows = 0;
    for (const auto& segment : dataset.Segments()) sealed_rows += segment.RowCount();

    CalibrateGpuCrossover(dataset, field_index, query, dimension, metric, top_k, sealed_rows);

    const bool gpu_may_run = sealed_rows <= UINT32_MAX && WantsGpu(sealed_rows, dimension);
    std::vector<uint32_t> gpu_candidates;
    if (gpu_may_run && !predicates.empty()) {
        uint32_t base_row = 0;
        for (const auto& segment : dataset.Segments()) {
            const auto& fields = segment.Fields();
            const auto* compressed = &segment.CompressedColumns();
            for (size_t row = 0; row < segment.RowCount(); ++row) {
                if (Matches(fields, row, compressed, predicates))
                    gpu_candidates.push_back(base_row + static_cast<uint32_t>(row));
            }
            base_row += static_cast<uint32_t>(segment.RowCount());
        }
    }
    const size_t gpu_candidate_count = predicates.empty() ? sealed_rows : gpu_candidates.size();
    bool gpu_used = false;
    std::vector<VectorSearchHit> gpu_hits;
    if (gpu_may_run && WantsGpu(gpu_candidate_count, dimension) &&
        PreloadVectorField(dataset, field_index)) {
        std::vector<float> distances;
        if (VectorGpuScore(dataset, field_index, query, dimension, metric, gpu_candidates, &distances)) {
            gpu_used = true;
            gpu_hits.reserve(distances.size());
            for (size_t i = 0; i < distances.size(); ++i) {
                if (!std::isfinite(distances[i])) continue;
                const uint64_t row = gpu_candidates.empty() ? i : gpu_candidates[i];
                gpu_hits.push_back({row, distances[i]});
            }
        }
    }
    struct Work {
        const std::vector<FieldVector>* fields;
        const std::vector<CompressedColumnView>* compressed;
        uint64_t base;
        std::vector<size_t> candidates;
        bool candidates_precomputed = false;
    };
    std::vector<Work> work;
    uint64_t base = 0;
    for (const auto& segment : dataset.Segments()) {
        if (field_index >= segment.Fields().size()) return false;
        if (!gpu_used) {
            Work item{&segment.Fields(), &segment.CompressedColumns(), base, {}, false};
            if (gpu_may_run && !predicates.empty()) {
                item.candidates_precomputed = true;
                const uint64_t end = base + segment.RowCount();
                auto first = std::lower_bound(gpu_candidates.begin(), gpu_candidates.end(), base);
                auto last = std::lower_bound(first, gpu_candidates.end(), end);
                item.candidates.reserve(static_cast<size_t>(last - first));
                for (; first != last; ++first) item.candidates.push_back(*first - base);
            }
            work.push_back(std::move(item));
        }
        base += segment.RowCount();
    }
    if (dataset.ActiveRowCount() != 0)
        work.push_back({&dataset.ActiveFields(), nullptr, base, {}, false});
    auto search_one = [&](Work item) {
        std::priority_queue<VectorSearchHit, std::vector<VectorSearchHit>, Worse> local;
        SearchField(*item.fields, field_index, item.base, query, dimension, top_k, metric,
                    query_norm_squared,
                    item.compressed,
                    predicates, item.candidates_precomputed ? &item.candidates : nullptr, &local);
        std::vector<VectorSearchHit> hits;
        while (!local.empty()) { hits.push_back(local.top()); local.pop(); }
        return hits;
    };
    std::vector<std::vector<VectorSearchHit>> partial_hits(work.size());
    ParallelFor(work.size(), [&](size_t index) {
        partial_hits[index] = search_one(std::move(work[index]));
    });
    std::priority_queue<VectorSearchHit, std::vector<VectorSearchHit>, Worse> heap;
    for (const auto& hit : gpu_hits) {
        if (heap.size() < top_k) heap.push(hit);
        else if (hit.distance < heap.top().distance ||
                 (hit.distance == heap.top().distance && hit.row_id < heap.top().row_id)) {
            heap.pop(); heap.push(hit);
        }
    }
    for (const auto& partial : partial_hits) {
        for (const auto& hit : partial) {
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

bool VectorSearchCandidates(const Dataset& dataset, size_t field_index,
                            const float* query, size_t dimension, size_t top_k,
                            VectorMetric metric, const std::vector<uint32_t>& requested,
                            std::vector<VectorSearchHit>* out,
                            const std::vector<VectorSearchPredicate>& predicates) {
    if (!out || !query || dimension == 0 || top_k == 0 ||
        field_index >= dataset.Fields().size() ||
        dataset.Fields()[field_index].Type() != FieldType::kVectorFloat32 ||
        dataset.VectorDimension(field_index) != dimension) return false;
    for (const auto& predicate : predicates)
        if (predicate.field_index >= dataset.Fields().size()) return false;
    for (size_t i = 0; i < dimension; ++i)
        if (!std::isfinite(query[i])) return false;

    std::vector<uint32_t> candidates = requested;
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    struct Work {
        const std::vector<FieldVector>* fields;
        const std::vector<CompressedColumnView>* compressed;
        uint64_t base;
        std::vector<size_t> rows;
        bool exact_all = false;
    };
    std::vector<Work> work;
    std::vector<uint32_t> filtered_global;
    uint64_t base = 0;
    for (const auto& segment : dataset.Segments()) {
        Work item{&segment.Fields(), &segment.CompressedColumns(), base, {}, false};
        const uint64_t end = base + segment.RowCount();
        auto first = std::lower_bound(candidates.begin(), candidates.end(), base);
        auto last = std::lower_bound(first, candidates.end(), end);
        item.rows.reserve(static_cast<size_t>(last - first));
        for (; first != last; ++first) {
            const size_t row = static_cast<size_t>(*first - base);
            if (predicates.empty() || Matches(segment.Fields(), row,
                                               &segment.CompressedColumns(), predicates)) {
                item.rows.push_back(row);
                filtered_global.push_back(*first);
            }
        }
        if (!item.rows.empty()) work.push_back(std::move(item));
        base = end;
    }

    std::priority_queue<VectorSearchHit, std::vector<VectorSearchHit>, Worse> heap;
    const bool gpu_used = WantsGpu(filtered_global.size(), dimension) &&
                          PreloadVectorField(dataset, field_index);
    if (gpu_used) {
        std::vector<float> distances;
        if (!VectorGpuScore(dataset, field_index, query, dimension, metric,
                            filtered_global, &distances)) return false;
        for (size_t i = 0; i < distances.size(); ++i) {
            const VectorSearchHit hit{filtered_global[i], distances[i]};
            if (!std::isfinite(hit.distance)) continue;
            if (heap.size() < top_k) heap.push(hit);
            else if (hit.distance < heap.top().distance ||
                     (hit.distance == heap.top().distance && hit.row_id < heap.top().row_id)) {
                heap.pop(); heap.push(hit);
            }
        }
        work.clear();
    }
    if (dataset.ActiveRowCount() != 0)
        work.push_back({&dataset.ActiveFields(), nullptr, base, {}, true});

    const float query_norm_squared = metric == VectorMetric::kCosine
        ? FastDot(query, query, dimension) : 0.0F;
    std::vector<std::vector<VectorSearchHit>> partial(work.size());
    ParallelFor(work.size(), [&](size_t index) {
        auto& item = work[index];
        std::priority_queue<VectorSearchHit, std::vector<VectorSearchHit>, Worse> local;
        SearchField(*item.fields, field_index, item.base, query, dimension, top_k,
                    metric, query_norm_squared, item.compressed, predicates,
                    item.exact_all ? nullptr : &item.rows, &local);
        while (!local.empty()) { partial[index].push_back(local.top()); local.pop(); }
    });
    for (const auto& part : partial) {
        for (const auto& hit : part) {
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

VectorSearchRuntimeStats GetVectorSearchRuntimeStats() {
    auto& state = RoutingState();
    std::lock_guard lock(state.mutex);
    return {VectorWorkerPool::Instance().Size(), state.crossover_elements,
            state.max_tested_elements, state.calibrated};
}

void ConfigureVectorSearchThreads(size_t threads) {
    configured_cpu_threads.store(threads, std::memory_order_relaxed);
}

}  // namespace mimicdb
