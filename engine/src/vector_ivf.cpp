#include "mimicdb/vector_ivf.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <numeric>
#include <memory>
#include <unordered_map>

#include "mimicdb/dataset.h"

namespace mimicdb {

bool VectorSearchCandidates(const Dataset&, size_t, const float*, size_t, size_t,
                            VectorMetric, const std::vector<uint32_t>&,
                            std::vector<VectorSearchHit>*,
                            const std::vector<VectorSearchPredicate>&);

namespace {
struct Key {
    const Dataset* dataset;
    size_t field;
    VectorMetric metric;
    bool operator==(const Key& other) const {
        return dataset == other.dataset && field == other.field && metric == other.metric;
    }
};
struct KeyHash {
    size_t operator()(const Key& key) const {
        return std::hash<const void*>{}(key.dataset) ^ (key.field << 4) ^
               static_cast<size_t>(key.metric);
    }
};
struct Index {
    size_t rows = 0;
    size_t source_rows = 0;
    size_t segments = 0;
    size_t dimension = 0;
    size_t routing_dimensions = 0;
    size_t centroids = 0;
    std::vector<uint32_t> routing_columns;
    std::vector<float> centers;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> row_ids;
    uint64_t builds = 0;
};

float RoutingDistance(const float* vector, const float* center,
                      const std::vector<uint32_t>& columns, VectorMetric metric) {
    float dot = 0.0F, left_norm = 0.0F, right_norm = 0.0F, l2 = 0.0F;
    for (size_t i = 0; i < columns.size(); ++i) {
        const float left = vector[columns[i]], right = center[i];
        if (metric == VectorMetric::kL2Squared) {
            const float delta = left - right;
            l2 += delta * delta;
        } else {
            dot += left * right;
            if (metric == VectorMetric::kCosine) {
                left_norm += left * left;
                right_norm += right * right;
            }
        }
    }
    if (metric == VectorMetric::kL2Squared) return l2;
    if (metric == VectorMetric::kDot) return -dot;
    if (left_norm <= 0.0F || right_norm <= 0.0F) return 1.0F;
    return 1.0F - dot / std::sqrt(left_norm * right_norm);
}

class Store {
public:
    static Store& Instance() { static Store store; return store; }

    bool Build(const Dataset& dataset, size_t field, VectorMetric metric) {
        if (field >= dataset.Fields().size() ||
            dataset.Fields()[field].Type() != FieldType::kVectorFloat32) return false;
        size_t rows = 0;
        for (const auto& segment : dataset.Segments()) rows += segment.RowCount();
        if (rows == 0 || rows > UINT32_MAX) return false;
        const size_t dimension = dataset.VectorDimension(field);
        Key key{&dataset, field, metric};
        uint64_t previous_builds = 0;
        {
            std::lock_guard lock(mutex_);
            auto found = indexes_.find(key);
            if (found != indexes_.end() && found->second->source_rows == rows &&
                found->second->segments == dataset.Segments().size() &&
                found->second->dimension == dimension) return true;
            if (found != indexes_.end()) previous_builds = found->second->builds;
        }

        auto next = std::make_shared<Index>();
        next->rows = rows;
        next->source_rows = rows;
        next->segments = dataset.Segments().size();
        next->dimension = dimension;
        next->routing_dimensions = std::min<size_t>(dimension, 96);
        size_t centroid_count = std::clamp<size_t>(
            static_cast<size_t>(std::sqrt(static_cast<double>(rows)) / 2.0), 32, 512);
        if (const char* value = std::getenv("MIMICDB_IVF_CENTROIDS")) {
            const size_t parsed = std::strtoull(value, nullptr, 10);
            if (parsed != 0) centroid_count = std::clamp<size_t>(parsed, 2, rows);
        }
        next->centroids = std::min(centroid_count, rows);
        next->routing_columns.resize(next->routing_dimensions);
        for (size_t i = 0; i < next->routing_dimensions; ++i)
            next->routing_columns[i] = static_cast<uint32_t>(i * dimension / next->routing_dimensions);

        std::vector<const float*> vectors;
        std::vector<uint32_t> vector_row_ids;
        vectors.reserve(rows);
        vector_row_ids.reserve(rows);
        uint64_t global_row = 0;
        for (const auto& segment : dataset.Segments()) {
            const auto& column = segment.Fields()[field];
            for (size_t row = 0; row < segment.RowCount(); ++row) {
                size_t stored_dimension = 0;
                const float* value = column.VectorFloat32(row, &stored_dimension);
                if (value && stored_dimension == dimension) {
                    vectors.push_back(value);
                    vector_row_ids.push_back(static_cast<uint32_t>(global_row));
                }
                ++global_row;
            }
        }
        if (vectors.empty()) return false;
        const size_t indexed_rows = vectors.size();
        next->centroids = std::min(next->centroids, indexed_rows);
        next->centers.resize(next->centroids * next->routing_dimensions);
        for (size_t center = 0; center < next->centroids; ++center) {
            const size_t source = center * indexed_rows / next->centroids;
            for (size_t d = 0; d < next->routing_dimensions; ++d)
                next->centers[center * next->routing_dimensions + d] =
                    vectors[source][next->routing_columns[d]];
        }

        const size_t sample_count = std::min<size_t>(indexed_rows, 8192);
        std::vector<uint32_t> assignments(sample_count);
        std::vector<float> sums(next->centers.size());
        std::vector<uint32_t> counts(next->centroids);
        for (size_t iteration = 0; iteration < 5; ++iteration) {
            std::fill(sums.begin(), sums.end(), 0.0F);
            std::fill(counts.begin(), counts.end(), 0);
            for (size_t sample = 0; sample < sample_count; ++sample) {
                const float* vector = vectors[sample * indexed_rows / sample_count];
                float best = std::numeric_limits<float>::infinity();
                uint32_t best_center = 0;
                for (size_t center = 0; center < next->centroids; ++center) {
                    const float distance = RoutingDistance(
                        vector, next->centers.data() + center * next->routing_dimensions,
                        next->routing_columns, metric);
                    if (distance < best) { best = distance; best_center = center; }
                }
                assignments[sample] = best_center;
                ++counts[best_center];
                for (size_t d = 0; d < next->routing_dimensions; ++d)
                    sums[best_center * next->routing_dimensions + d] +=
                        vector[next->routing_columns[d]];
            }
            for (size_t center = 0; center < next->centroids; ++center) {
                if (counts[center] == 0) continue;
                for (size_t d = 0; d < next->routing_dimensions; ++d)
                    next->centers[center * next->routing_dimensions + d] =
                        sums[center * next->routing_dimensions + d] / counts[center];
            }
        }

        std::vector<uint32_t> all_assignments(indexed_rows);
        next->offsets.assign(next->centroids + 1, 0);
        for (size_t row = 0; row < indexed_rows; ++row) {
            float best = std::numeric_limits<float>::infinity();
            uint32_t best_center = 0;
            for (size_t center = 0; center < next->centroids; ++center) {
                const float distance = RoutingDistance(
                    vectors[row], next->centers.data() + center * next->routing_dimensions,
                    next->routing_columns, metric);
                if (distance < best) { best = distance; best_center = center; }
            }
            all_assignments[row] = best_center;
            ++next->offsets[best_center + 1];
        }
        std::partial_sum(next->offsets.begin(), next->offsets.end(), next->offsets.begin());
        next->row_ids.resize(indexed_rows);
        std::vector<uint32_t> write = next->offsets;
        for (size_t row = 0; row < indexed_rows; ++row)
            next->row_ids[write[all_assignments[row]]++] = vector_row_ids[row];
        next->rows = indexed_rows;
        next->builds = previous_builds + 1;
        {
            std::lock_guard lock(mutex_);
            indexes_[key] = std::move(next);
        }
        return true;
    }

    bool Candidates(const Dataset& dataset, size_t field, VectorMetric metric,
                    const float* query, size_t probes, std::vector<uint32_t>* out,
                    IvfSearchStats* stats) {
        if (!Build(dataset, field, metric)) return false;
        std::shared_ptr<const Index> held;
        {
            std::lock_guard lock(mutex_);
            const auto found = indexes_.find({&dataset, field, metric});
            if (found == indexes_.end()) return false;
            held = found->second;
        }
        const Index& index = *held;
        if (probes == 0) probes = std::max<size_t>(1, index.centroids / 3);
        probes = std::min(probes, index.centroids);
        std::vector<std::pair<float, uint32_t>> ranked(index.centroids);
        for (uint32_t center = 0; center < index.centroids; ++center)
            ranked[center] = {RoutingDistance(
                query, index.centers.data() + center * index.routing_dimensions,
                index.routing_columns, metric), center};
        if (probes < ranked.size())
            std::nth_element(ranked.begin(), ranked.begin() + probes, ranked.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
        size_t count = 0;
        for (size_t i = 0; i < probes; ++i)
            count += index.offsets[ranked[i].second + 1] - index.offsets[ranked[i].second];
        out->clear(); out->reserve(count);
        for (size_t i = 0; i < probes; ++i) {
            const uint32_t center = ranked[i].second;
            out->insert(out->end(), index.row_ids.begin() + index.offsets[center],
                        index.row_ids.begin() + index.offsets[center + 1]);
        }
        if (stats) *stats = {index.rows, index.centroids, probes, out->size(),
                             index.routing_dimensions, index.builds};
        return true;
    }

    IvfSearchStats Stats(const Dataset& dataset, size_t field, VectorMetric metric) {
        std::lock_guard lock(mutex_);
        const auto found = indexes_.find({&dataset, field, metric});
        if (found == indexes_.end()) return {};
        const auto& i = *found->second;
        return {i.rows, i.centroids, 0, 0, i.routing_dimensions, i.builds};
    }
    void Release(const Dataset& dataset) {
        std::lock_guard lock(mutex_);
        for (auto it = indexes_.begin(); it != indexes_.end();)
            if (it->first.dataset == &dataset) it = indexes_.erase(it); else ++it;
    }
private:
    std::mutex mutex_;
    std::unordered_map<Key, std::shared_ptr<const Index>, KeyHash> indexes_;
};
}  // namespace

bool BuildVectorIvf(const Dataset& dataset, size_t field_index, VectorMetric metric) {
    return Store::Instance().Build(dataset, field_index, metric);
}
bool VectorSearchIvf(const Dataset& dataset, size_t field_index, const float* query,
                     size_t dimension, size_t top_k, VectorMetric metric,
                     size_t probes, std::vector<VectorSearchHit>* out,
                     const std::vector<VectorSearchPredicate>& predicates) {
    std::vector<uint32_t> candidates;
    if (!Store::Instance().Candidates(dataset, field_index, metric, query, probes,
                                      &candidates, nullptr)) {
        size_t sealed_rows = 0;
        for (const auto& segment : dataset.Segments()) sealed_rows += segment.RowCount();
        if (sealed_rows != 0) return false;
    }
    return VectorSearchCandidates(dataset, field_index, query, dimension, top_k, metric,
                                  candidates, out, predicates);
}
IvfSearchStats GetVectorIvfStats(const Dataset& dataset, size_t field_index,
                                 VectorMetric metric) {
    return Store::Instance().Stats(dataset, field_index, metric);
}
void ReleaseVectorIvfDataset(const Dataset& dataset) { Store::Instance().Release(dataset); }

}  // namespace mimicdb
