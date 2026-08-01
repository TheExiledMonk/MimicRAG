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

int main(int argc, char** argv) {
    const size_t rows = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000;
    const size_t dimension = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 384;
    const size_t top_k = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 10;
    const size_t iterations = std::max<size_t>(
        1, argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 5);
    mimicdb::Dataset dataset("vector_bench");
    dataset.AddField(mimicdb::FieldVector("embedding", mimicdb::FieldType::kVectorFloat32));
    dataset.AddField(mimicdb::FieldVector("tenant", mimicdb::FieldType::kInt32));
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
    std::vector<float> values(dimension);
    for (size_t row = 0; row < rows; ++row) {
        for (auto& value : values) value = dist(rng);
        dataset.Append({mimicdb::FieldValue::VectorFloat32(values),
                        mimicdb::FieldValue::Int32(static_cast<int32_t>(row % 100))});
    }
    for (auto& value : values) value = dist(rng);
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
              << " iterations=" << iterations << " seconds=" << seconds
              << " min_seconds=" << exact_timings.front()
              << " p95_seconds=" << exact_timings[std::min(exact_timings.size() - 1,
                  static_cast<size_t>(exact_timings.size() * 0.95))]
              << " vectors_per_sec=" << vectors_per_sec
              << " effective_bytes_per_sec=" << bandwidth << " hits=" << hits.size() << "\n";

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
