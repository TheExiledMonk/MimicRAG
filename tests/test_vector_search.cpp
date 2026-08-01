#include <cassert>
#include <cmath>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <future>
#include <fstream>

#include "mimicdb/dataset.h"
#include "mimicdb/vector_search.h"
#include "mimicdb/segment_io.h"
#include "mimicdb/vector_gpu.h"
#include "mimicdb/vector_ivf.h"

int main() {
    mimicdb::Dataset dataset("vectors");
    dataset.AddField(mimicdb::FieldVector("embedding", mimicdb::FieldType::kVectorFloat32));
    assert(dataset.Append({mimicdb::FieldValue::VectorFloat32({1.0F, 0.0F, 0.0F})}));
    assert(dataset.Append({mimicdb::FieldValue::VectorFloat32({0.0F, 1.0F, 0.0F})}));
    assert(dataset.Append({mimicdb::FieldValue::VectorFloat32({0.9F, 0.1F, 0.0F})}));
    assert(!dataset.Append({mimicdb::FieldValue::VectorFloat32({1.0F, 0.0F})}));
    const float query[] = {1.0F, 0.0F, 0.0F};
    std::vector<mimicdb::VectorSearchHit> hits;
    assert(mimicdb::VectorSearch(dataset, 0, query, 3, 2,
                                 mimicdb::VectorMetric::kCosine, &hits));
    assert(hits.size() == 2);
    assert(hits[0].row_id == 0 && std::fabs(hits[0].distance) < 1e-6F);
    assert(hits[1].row_id == 2);
    assert(mimicdb::VectorDistance(query, query, 3, mimicdb::VectorMetric::kL2Squared) == 0.0F);

    mimicdb::Dataset filtered("filtered");
    filtered.AddField(mimicdb::FieldVector("embedding", mimicdb::FieldType::kVectorFloat32));
    filtered.AddField(mimicdb::FieldVector("tenant", mimicdb::FieldType::kInt32));
    for (int i = 0; i < 4100; ++i) {
        const float x = i == 4099 ? 1.0F : 0.0F;
        const auto vector = i == 100
            ? mimicdb::FieldValue::Null(mimicdb::FieldType::kVectorFloat32)
            : mimicdb::FieldValue::VectorFloat32({x, 1.0F - x});
        assert(filtered.Append({vector,
                                mimicdb::FieldValue::Int32(i == 4099 ? 7 : 3)}));
    }
    const float filtered_query[] = {1.0F, 0.0F};
    std::vector<mimicdb::VectorSearchPredicate> predicates = {
        {1, mimicdb::CompareOp::kEq, 7.0},
    };
    assert(mimicdb::VectorSearch(filtered, 0, filtered_query, 2, 1,
                                 mimicdb::VectorMetric::kCosine, &hits, predicates));
    assert(hits.size() == 1 && hits[0].row_id == 4099);

    predicates[0].value = 99.0;
    assert(mimicdb::VectorSearch(filtered, 0, filtered_query, 2, 1,
                                 mimicdb::VectorMetric::kCosine, &hits, predicates));
    assert(hits.empty());

    predicates[0].value = 7.0;
    setenv("MIMICDB_VECTOR_BACKEND", "vulkan", 1);
    assert(mimicdb::PreloadVectorField(filtered, 0) ||
           !mimicdb::GetVectorGpuStats().available);
    assert(mimicdb::VectorSearch(filtered, 0, filtered_query, 2, 1,
                                 mimicdb::VectorMetric::kCosine, &hits, predicates));
    assert(hits.size() == 1 && hits[0].row_id == 4099);
    unsetenv("MIMICDB_VECTOR_BACKEND");

    setenv("MIMICDB_VECTOR_BACKEND", "cpu", 1);
    std::vector<std::future<bool>> concurrent;
    for (size_t task = 0; task < 4; ++task) {
        concurrent.push_back(std::async(std::launch::async, [&] {
            std::vector<mimicdb::VectorSearchHit> local_hits;
            return mimicdb::VectorSearch(filtered, 0, filtered_query, 2, 1,
                                          mimicdb::VectorMetric::kCosine,
                                          &local_hits, predicates) &&
                   local_hits.size() == 1 && local_hits[0].row_id == 4099;
        }));
    }
    for (auto& result : concurrent) assert(result.get());
    unsetenv("MIMICDB_VECTOR_BACKEND");

    assert(mimicdb::BuildVectorIvf(filtered, 0, mimicdb::VectorMetric::kCosine));
    const auto ivf_stats = mimicdb::GetVectorIvfStats(
        filtered, 0, mimicdb::VectorMetric::kCosine);
    const auto ivf_path = std::filesystem::temp_directory_path() / "mimicdb_vector_ivf_test.bin";
    std::filesystem::remove(ivf_path);
    assert(mimicdb::SaveVectorIvf(filtered, 0, mimicdb::VectorMetric::kCosine,
                                  ivf_path.c_str()));
    std::vector<mimicdb::VectorSearchHit> exact_all;
    std::vector<mimicdb::VectorSearchHit> ivf_all;
    assert(mimicdb::VectorSearchIvf(filtered, 0, filtered_query, 2, 1,
                                    mimicdb::VectorMetric::kCosine, 0, &hits, predicates));
    assert(hits.size() == 1 && hits[0].row_id == 4099);
    const auto filtered_ivf_stats = mimicdb::GetVectorIvfStats(
        filtered, 0, mimicdb::VectorMetric::kCosine);
    assert(filtered_ivf_stats.lists_pruned > 0 && filtered_ivf_stats.candidates == 0);
    assert(mimicdb::VectorSearch(filtered, 0, filtered_query, 2, 10,
                                 mimicdb::VectorMetric::kCosine, &exact_all));
    assert(mimicdb::VectorSearchIvf(filtered, 0, filtered_query, 2, 10,
                                    mimicdb::VectorMetric::kCosine,
                                    ivf_stats.centroid_count, &ivf_all));
    assert(exact_all.size() == ivf_all.size());
    for (size_t i = 0; i < exact_all.size(); ++i)
        assert(exact_all[i].row_id == ivf_all[i].row_id);
    assert(filtered.Append({mimicdb::FieldValue::VectorFloat32({0.8F, 0.2F}),
                            mimicdb::FieldValue::Int32(7)}));
    const float appended_query[] = {0.8F, 0.2F};
    assert(mimicdb::VectorSearchIvf(filtered, 0, appended_query, 2, 1,
                                    mimicdb::VectorMetric::kCosine,
                                    ivf_stats.centroid_count, &hits));
    assert(hits.size() == 1 && hits[0].row_id == 4100);
    for (const auto metric : {mimicdb::VectorMetric::kDot,
                              mimicdb::VectorMetric::kL2Squared}) {
        assert(mimicdb::BuildVectorIvf(filtered, 0, metric));
        const auto metric_stats = mimicdb::GetVectorIvfStats(filtered, 0, metric);
        assert(mimicdb::VectorSearch(filtered, 0, appended_query, 2, 10,
                                     metric, &exact_all));
        assert(mimicdb::VectorSearchIvf(filtered, 0, appended_query, 2, 10,
                                        metric, metric_stats.centroid_count, &ivf_all));
        assert(exact_all.size() == ivf_all.size());
        for (size_t i = 0; i < exact_all.size(); ++i)
            assert(exact_all[i].row_id == ivf_all[i].row_id);
    }

    mimicdb::Dataset noisy("noisy");
    noisy.AddField(mimicdb::FieldVector("embedding", mimicdb::FieldType::kVectorFloat32));
    for (size_t row = 0; row < 4100; ++row) {
        std::vector<float> vector(8);
        for (size_t d = 0; d < vector.size(); ++d)
            vector[d] = std::sin(static_cast<float>((row + 1) * (d + 3)));
        assert(noisy.Append({mimicdb::FieldValue::VectorFloat32(std::move(vector))}));
    }
    const float noisy_query[] = {0.2F, -0.7F, 0.4F, 0.1F, -0.3F, 0.8F, -0.5F, 0.6F};
    setenv("MIMICDB_IVF_MAX_ASSIGNMENT_DISTANCE", "0.01", 1);
    assert(mimicdb::VectorSearch(noisy, 0, noisy_query, 8, 10,
                                 mimicdb::VectorMetric::kCosine, &exact_all));
    assert(mimicdb::VectorSearchIvf(noisy, 0, noisy_query, 8, 10,
                                    mimicdb::VectorMetric::kCosine, 0, &ivf_all));
    assert(mimicdb::GetVectorIvfStats(noisy, 0, mimicdb::VectorMetric::kCosine)
               .exact_fallback);
    assert(exact_all.size() == ivf_all.size());
    for (size_t i = 0; i < exact_all.size(); ++i)
        assert(exact_all[i].row_id == ivf_all[i].row_id);
    unsetenv("MIMICDB_IVF_MAX_ASSIGNMENT_DISTANCE");

    const auto path = std::filesystem::temp_directory_path() / "mimicdb_vector_segment_test.bin";
    std::filesystem::remove(path);
    assert(mimicdb::SegmentWriter(path.string()).Write(filtered.Segments()[0]));
    mimicdb::Segment recovered(0, std::vector<mimicdb::FieldVector>{});
    assert(mimicdb::SegmentReader(path.string()).Read(&recovered));
    mimicdb::Dataset restored("restored");
    restored.AddField(mimicdb::FieldVector("embedding", mimicdb::FieldType::kVectorFloat32));
    restored.AddField(mimicdb::FieldVector("tenant", mimicdb::FieldType::kInt32));
    assert(restored.AddRecoveredSegment(std::move(recovered)));
    assert(restored.VectorDimension(0) == 2);
    assert(mimicdb::LoadVectorIvf(restored, 0, mimicdb::VectorMetric::kCosine,
                                  ivf_path.c_str()));
    assert(mimicdb::VectorIvfReady(restored, 0, mimicdb::VectorMetric::kCosine));
    assert(mimicdb::VectorSearch(restored, 0, filtered_query, 2, 1,
                                 mimicdb::VectorMetric::kCosine, &hits));
    {
        std::fstream corrupt(ivf_path, std::ios::binary | std::ios::in | std::ios::out);
        corrupt.seekg(-1, std::ios::end);
        char byte = 0;
        corrupt.read(&byte, 1);
        byte ^= '\x7f';
        corrupt.seekp(-1, std::ios::end);
        corrupt.write(&byte, 1);
    }
    mimicdb::Dataset rejected("rejected");
    rejected.AddField(mimicdb::FieldVector("embedding", mimicdb::FieldType::kVectorFloat32));
    rejected.AddField(mimicdb::FieldVector("tenant", mimicdb::FieldType::kInt32));
    mimicdb::Segment rejected_segment(0, std::vector<mimicdb::FieldVector>{});
    assert(mimicdb::SegmentReader(path.string()).Read(&rejected_segment));
    assert(rejected.AddRecoveredSegment(std::move(rejected_segment)));
    assert(!mimicdb::LoadVectorIvf(rejected, 0, mimicdb::VectorMetric::kCosine,
                                   ivf_path.c_str()));
    std::filesystem::remove(path);
    std::filesystem::remove(ivf_path);
    return 0;
}
