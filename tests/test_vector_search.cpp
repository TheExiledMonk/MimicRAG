#include <cassert>
#include <cmath>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <future>

#include "mimicdb/dataset.h"
#include "mimicdb/vector_search.h"
#include "mimicdb/segment_io.h"
#include "mimicdb/vector_gpu.h"

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
        assert(filtered.Append({mimicdb::FieldValue::VectorFloat32({x, 1.0F - x}),
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
    assert(mimicdb::VectorSearch(restored, 0, filtered_query, 2, 1,
                                 mimicdb::VectorMetric::kCosine, &hits));
    std::filesystem::remove(path);
    return 0;
}
