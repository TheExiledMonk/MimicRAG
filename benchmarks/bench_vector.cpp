#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

#include "mimicdb/dataset.h"
#include "mimicdb/vector_search.h"

int main(int argc, char** argv) {
    const size_t rows = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000;
    const size_t dimension = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 384;
    const size_t top_k = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 10;
    mimicdb::Dataset dataset("vector_bench");
    dataset.AddField(mimicdb::FieldVector("embedding", mimicdb::FieldType::kVectorFloat32));
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
    std::vector<float> values(dimension);
    for (size_t row = 0; row < rows; ++row) {
        for (auto& value : values) value = dist(rng);
        dataset.Append({mimicdb::FieldValue::VectorFloat32(values)});
    }
    for (auto& value : values) value = dist(rng);
    std::vector<mimicdb::VectorSearchHit> hits;
    const auto start = std::chrono::steady_clock::now();
    const bool ok = mimicdb::VectorSearch(dataset, 0, values.data(), dimension, top_k,
                                           mimicdb::VectorMetric::kCosine, &hits);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!ok) return 1;
    const double vectors_per_sec = static_cast<double>(rows) / seconds;
    const double bandwidth = static_cast<double>(rows * dimension * sizeof(float)) / seconds;
    std::cout << "benchmark=vector_exact metric=cosine rows=" << rows
              << " dimension=" << dimension << " top_k=" << top_k
              << " seconds=" << seconds << " vectors_per_sec=" << vectors_per_sec
              << " effective_bytes_per_sec=" << bandwidth << " hits=" << hits.size() << "\n";
    return 0;
}
