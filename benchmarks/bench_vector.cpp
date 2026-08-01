#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

#include "mimicdb/dataset.h"
#include "mimicdb/vector_search.h"
#include "mimicdb/vector_gpu.h"
#include "mimicdb/vector_ivf.h"

int main(int argc, char** argv) {
    const size_t rows = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000;
    const size_t dimension = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 384;
    const size_t top_k = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 10;
    const size_t iterations = std::max<size_t>(
        1, argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 5);
    const size_t ivf_probes = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 0;
    const size_t cluster_count = argc > 6 ? std::strtoull(argv[6], nullptr, 10) : 0;
    mimicdb::Dataset dataset("vector_bench");
    dataset.AddField(mimicdb::FieldVector("embedding", mimicdb::FieldType::kVectorFloat32));
    dataset.AddField(mimicdb::FieldVector("tenant", mimicdb::FieldType::kInt32));
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
    std::normal_distribution<float> noise(0.0F, 0.08F);
    std::vector<float> cluster_centers(cluster_count * dimension);
    for (auto& value : cluster_centers) value = dist(rng);
    std::vector<float> values(dimension);
    for (size_t row = 0; row < rows; ++row) {
        if (cluster_count == 0) {
            for (auto& value : values) value = dist(rng);
        } else {
            const float* center = cluster_centers.data() + (row % cluster_count) * dimension;
            for (size_t d = 0; d < dimension; ++d) values[d] = center[d] + noise(rng);
        }
        dataset.Append({mimicdb::FieldValue::VectorFloat32(values),
                        mimicdb::FieldValue::Int32(static_cast<int32_t>(row % 100))});
    }
    if (cluster_count == 0) {
        for (auto& value : values) value = dist(rng);
    } else {
        const float* center = cluster_centers.data() + (cluster_count / 2) * dimension;
        for (size_t d = 0; d < dimension; ++d) values[d] = center[d] + noise(rng);
    }
    std::vector<mimicdb::VectorSearchHit> hits;
    const auto preload_start = std::chrono::steady_clock::now();
    const bool gpu_resident = mimicdb::PreloadVectorField(dataset, 0);
    const double preload_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - preload_start).count();
    const auto calibration_start = std::chrono::steady_clock::now();
    const bool calibration_ok = mimicdb::VectorSearch(
        dataset, 0, values.data(), dimension, top_k,
        mimicdb::VectorMetric::kCosine, &hits);
    const double calibration_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - calibration_start).count();
    if (!calibration_ok) return 1;
    std::vector<double> exact_timings;
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = mimicdb::VectorSearch(dataset, 0, values.data(), dimension, top_k,
                                               mimicdb::VectorMetric::kCosine, &hits);
        exact_timings.push_back(std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count());
        if (!ok) return 1;
    }
    std::sort(exact_timings.begin(), exact_timings.end());
    const double seconds = exact_timings[exact_timings.size() / 2];
    const double vectors_per_sec = static_cast<double>(rows) / seconds;
    const double bandwidth = static_cast<double>(rows * dimension * sizeof(float)) / seconds;
    std::cout << "benchmark=vector_exact metric=cosine rows=" << rows
              << " dimension=" << dimension << " top_k=" << top_k
              << " data_clusters=" << cluster_count
              << " iterations=" << iterations << " seconds=" << seconds
              << " min_seconds=" << exact_timings.front()
              << " p95_seconds=" << exact_timings[std::min(exact_timings.size() - 1,
                  static_cast<size_t>(exact_timings.size() * 0.95))]
              << " vectors_per_sec=" << vectors_per_sec
              << " effective_bytes_per_sec=" << bandwidth << " hits=" << hits.size() << "\n";
    const auto exact_hits = hits;

    const auto ivf_build_start = std::chrono::steady_clock::now();
    if (!mimicdb::BuildVectorIvf(dataset, 0, mimicdb::VectorMetric::kCosine)) return 1;
    const double ivf_build_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - ivf_build_start).count();
    std::vector<double> ivf_timings;
    std::vector<mimicdb::VectorSearchHit> ivf_hits;
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        if (!mimicdb::VectorSearchIvf(dataset, 0, values.data(), dimension, top_k,
                                      mimicdb::VectorMetric::kCosine, ivf_probes,
                                      &ivf_hits)) return 1;
        ivf_timings.push_back(std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count());
    }
    std::sort(ivf_timings.begin(), ivf_timings.end());
    size_t overlap = 0;
    for (const auto& expected : exact_hits)
        for (const auto& actual : ivf_hits)
            if (expected.row_id == actual.row_id) { ++overlap; break; }
    const auto ivf = mimicdb::GetVectorIvfStats(dataset, 0, mimicdb::VectorMetric::kCosine);
    std::cout << "benchmark=vector_ivf metric=cosine rows=" << rows
              << " dimension=" << dimension << " top_k=" << top_k
              << " data_clusters=" << cluster_count
              << " centroids=" << ivf.centroid_count << " probes="
              << (ivf_probes == 0 ? std::max<size_t>(1, ivf.centroid_count / 3) : ivf_probes)
              << " routing_dimensions=" << ivf.routing_dimensions
              << " iterations=" << iterations << " seconds="
              << ivf_timings[ivf_timings.size() / 2]
              << " min_seconds=" << ivf_timings.front()
              << " p95_seconds=" << ivf_timings[std::min(ivf_timings.size() - 1,
                  static_cast<size_t>(ivf_timings.size() * 0.95))]
              << " recall_at_k=" << static_cast<double>(overlap) / exact_hits.size()
              << " build_seconds=" << ivf_build_seconds << " hits=" << ivf_hits.size() << "\n";

    const std::vector<mimicdb::VectorSearchPredicate> predicates = {
        {1, mimicdb::CompareOp::kEq, 0.0},
    };
    std::vector<double> filtered_timings;
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto filtered_start = std::chrono::steady_clock::now();
        const bool filtered_ok = mimicdb::VectorSearch(
            dataset, 0, values.data(), dimension, top_k, mimicdb::VectorMetric::kCosine,
            &hits, predicates);
        filtered_timings.push_back(std::chrono::duration<double>(
            std::chrono::steady_clock::now() - filtered_start).count());
        if (!filtered_ok) return 1;
    }
    std::sort(filtered_timings.begin(), filtered_timings.end());
    const double filtered_seconds = filtered_timings[filtered_timings.size() / 2];
    const size_t candidates = (rows + 99) / 100;
    std::cout << "benchmark=vector_filtered_exact metric=cosine rows=" << rows
              << " candidates=" << candidates << " selectivity=0.01"
              << " dimension=" << dimension << " top_k=" << top_k
              << " iterations=" << iterations << " seconds=" << filtered_seconds
              << " min_seconds=" << filtered_timings.front()
              << " p95_seconds=" << filtered_timings[std::min(filtered_timings.size() - 1,
                  static_cast<size_t>(filtered_timings.size() * 0.95))]
              << " candidate_vectors_per_sec="
              << static_cast<double>(candidates) / filtered_seconds
              << " hits=" << hits.size() << "\n";
    const auto gpu = mimicdb::GetVectorGpuStats();
    const auto runtime = mimicdb::GetVectorSearchRuntimeStats();
    std::cout << "benchmark=vector_gpu_residency available=" << gpu.available
              << " preloaded=" << gpu_resident << " preload_seconds=" << preload_seconds
              << " resident_bytes=" << gpu.resident_bytes << " uploads=" << gpu.uploads
              << " gpu_searches=" << gpu.searches
              << " device=\"" << gpu.device_name << "\"\n";
    std::cout << "benchmark=vector_router cpu_threads=" << runtime.cpu_threads
              << " gpu_calibrated=" << runtime.gpu_calibrated
              << " gpu_crossover_elements=" << runtime.gpu_crossover_elements
              << " calibration_max_elements=" << runtime.calibration_max_elements
              << " calibration_seconds=" << calibration_seconds << "\n";
    return 0;
}
