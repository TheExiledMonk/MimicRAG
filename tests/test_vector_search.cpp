#include <cassert>
#include <cmath>
#include <vector>

#include "mimicdb/dataset.h"
#include "mimicdb/vector_search.h"

int main() {
    mimicdb::Dataset dataset("vectors");
    dataset.AddField(mimicdb::FieldVector("embedding", mimicdb::FieldType::kVectorFloat32));
    assert(dataset.Append({mimicdb::FieldValue::VectorFloat32({1.0F, 0.0F, 0.0F})}));
    assert(dataset.Append({mimicdb::FieldValue::VectorFloat32({0.0F, 1.0F, 0.0F})}));
    assert(dataset.Append({mimicdb::FieldValue::VectorFloat32({0.9F, 0.1F, 0.0F})}));
    const float query[] = {1.0F, 0.0F, 0.0F};
    std::vector<mimicdb::VectorSearchHit> hits;
    assert(mimicdb::VectorSearch(dataset, 0, query, 3, 2,
                                 mimicdb::VectorMetric::kCosine, &hits));
    assert(hits.size() == 2);
    assert(hits[0].row_id == 0 && std::fabs(hits[0].distance) < 1e-6F);
    assert(hits[1].row_id == 2);
    assert(mimicdb::VectorDistance(query, query, 3, mimicdb::VectorMetric::kL2Squared) == 0.0F);
    return 0;
}
