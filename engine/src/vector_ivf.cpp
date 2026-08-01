#include "mimicdb/vector_ivf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <thread>
#include <unordered_map>

#include "mimicdb/dataset.h"
#include "mimicdb/compression.h"

namespace mimicdb {
bool VectorSearchCandidates(const Dataset&, size_t, const float*, size_t, size_t,
                            VectorMetric, const std::vector<uint32_t>&,
                            std::vector<VectorSearchHit>*,
                            const std::vector<VectorSearchPredicate>&);

namespace {
using Clock = std::chrono::steady_clock;

struct Key {
    const Dataset* dataset;
    size_t field;
    VectorMetric metric;
    bool operator==(const Key& x) const {
        return dataset == x.dataset && field == x.field && metric == x.metric;
    }
};
struct KeyHash {
    size_t operator()(const Key& x) const {
        return std::hash<const void*>{}(x.dataset) ^ (x.field << 4) ^ size_t(x.metric);
    }
};
struct Bounds {
    bool numeric = false;
    std::vector<double> minimum;
    std::vector<double> maximum;
};
struct Index {
    size_t rows = 0, source_rows = 0, segments = 0, dimension = 0;
    size_t routing_dimensions = 0, centroids = 0;
    std::vector<uint32_t> routing_columns, offsets, row_ids;
    std::vector<float> centers, sketch_scales;
    std::vector<float> packed_vectors;
    std::vector<float> inverse_norms, norm_squared;
    std::vector<int8_t> sketches;  // list-ordered, row-major routing projection
    std::vector<Bounds> bounds;
    uint64_t builds = 0;
    double build_seconds = 0.0;
    double mean_assignment_distance = 0.0;
};
struct ActiveIndex {
    size_t rows=0, dimension=0;
    std::vector<uint32_t> offsets, positions;
    std::vector<float> vectors, inverse_norms, norm_squared;
};

size_t ThreadCount() {
    size_t result = std::max(1U, std::thread::hardware_concurrency());
    if (const char* value = std::getenv("MIMICDB_VECTOR_CPU_THREADS")) {
        const size_t parsed = std::strtoull(value, nullptr, 10);
        if (parsed) result = parsed;
    }
    return result;
}
template <class F> void Parallel(size_t count, F function) {
    if (count < 8192) { for (size_t i = 0; i < count; ++i) function(i); return; }
    const size_t workers = std::min(count, ThreadCount());
    if (workers < 2) { for (size_t i = 0; i < count; ++i) function(i); return; }
    std::atomic<size_t> next{0};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < workers; ++t) threads.emplace_back([&] {
        for (;;) { const size_t i = next.fetch_add(1); if (i >= count) break; function(i); }
    });
    for (auto& thread : threads) thread.join();
}

bool Numeric(const Segment& segment, size_t field, size_t row, double* value) {
    if (field >= segment.CompressedColumns().size()) return false;
    return ReadNumericValue(segment.CompressedColumns()[field], row, value);
}

float Distance(const float* vector, const float* center,
               const std::vector<uint32_t>& columns, VectorMetric metric) {
    float dot = 0, ln = 0, rn = 0, l2 = 0;
    for (size_t i = 0; i < columns.size(); ++i) {
        const float a = vector[columns[i]], b = center[i];
        if (metric == VectorMetric::kL2Squared) { const float d = a - b; l2 += d * d; }
        else { dot += a * b; if (metric == VectorMetric::kCosine) { ln += a*a; rn += b*b; } }
    }
    if (metric == VectorMetric::kL2Squared) return l2;
    if (metric == VectorMetric::kDot) return -dot;
    return ln <= 0 || rn <= 0 ? 1.0F : 1.0F - dot / std::sqrt(ln * rn);
}

bool MayMatch(double minimum, double maximum, CompareOp op, double value) {
    switch (op) {
        case CompareOp::kEq: return value >= minimum && value <= maximum;
        case CompareOp::kNe: return minimum != maximum || minimum != value;
        case CompareOp::kLt: return minimum < value;
        case CompareOp::kLe: return minimum <= value;
        case CompareOp::kGt: return maximum > value;
        case CompareOp::kGe: return maximum >= value;
    }
    return true;
}

class Store {
public:
    static Store& Instance() { static Store value; return value; }

    bool Build(const Dataset& dataset, size_t field, VectorMetric metric) {
        const auto started = Clock::now();
        if (field >= dataset.Fields().size() ||
            dataset.Fields()[field].Type() != FieldType::kVectorFloat32) return false;
        size_t source_rows = 0;
        for (const auto& segment : dataset.Segments()) source_rows += segment.RowCount();
        if (!source_rows || source_rows > UINT32_MAX) return false;
        const size_t dimension = dataset.VectorDimension(field);
        const Key key{&dataset, field, metric};
        uint64_t builds = 0;
        {
            std::lock_guard lock(mutex_);
            auto found = indexes_.find(key);
            if (found != indexes_.end() && found->second->source_rows == source_rows &&
                found->second->segments == dataset.Segments().size() &&
                found->second->dimension == dimension) return true;
            if (found != indexes_.end()) builds = found->second->builds;
        }

        auto index = std::make_shared<Index>();
        index->source_rows = source_rows; index->segments = dataset.Segments().size();
        index->dimension = dimension; index->builds = builds + 1;
        size_t routing_dimensions = std::min<size_t>(dimension, 32);
        if (const char* value = std::getenv("MIMICDB_IVF_ROUTING_DIMS")) {
            const size_t parsed = std::strtoull(value, nullptr, 10);
            if (parsed) routing_dimensions = std::min(parsed, dimension);
        }
        index->routing_dimensions = routing_dimensions;

        std::vector<const float*> vectors;
        std::vector<uint32_t> global_ids;
        vectors.reserve(source_rows); global_ids.reserve(source_rows);
        uint32_t global = 0;
        for (const auto& segment : dataset.Segments()) {
            const auto& column = segment.Fields()[field];
            for (size_t row = 0; row < segment.RowCount(); ++row, ++global) {
                size_t stored = 0; const float* vector = column.VectorFloat32(row, &stored);
                if (vector && stored == dimension) { vectors.push_back(vector); global_ids.push_back(global); }
            }
        }
        if (vectors.empty()) return false;
        index->rows = vectors.size();

        // Select dimensions by sampled variance instead of fixed spacing.
        const size_t variance_sample = std::min<size_t>(vectors.size(), 4096);
        std::vector<std::pair<double, uint32_t>> variance(dimension);
        for (size_t d = 0; d < dimension; ++d) {
            double sum = 0, square = 0;
            for (size_t s = 0; s < variance_sample; ++s) {
                const double x = vectors[s * vectors.size() / variance_sample][d];
                sum += x; square += x*x;
            }
            variance[d] = {square / variance_sample - std::pow(sum / variance_sample, 2), uint32_t(d)};
        }
        std::partial_sort(variance.begin(), variance.begin() + routing_dimensions, variance.end(),
                          [](auto a, auto b) { return a.first > b.first; });
        index->routing_columns.resize(routing_dimensions);
        for (size_t d = 0; d < routing_dimensions; ++d) index->routing_columns[d] = variance[d].second;

        size_t centers = std::clamp<size_t>(size_t(std::sqrt(double(vectors.size())) / 2), 32, 512);
        if (const char* value = std::getenv("MIMICDB_IVF_CENTROIDS")) {
            const size_t parsed = std::strtoull(value, nullptr, 10);
            if (parsed) centers = std::clamp<size_t>(parsed, 2, vectors.size());
        }
        index->centroids = std::min(centers, vectors.size());
        index->centers.resize(index->centroids * routing_dimensions);
        // Deterministic far-apart sampling avoids adjacent/order-biased seeds.
        const uint64_t stride = 2654435761ULL;
        for (size_t c = 0; c < index->centroids; ++c) {
            const float* source = vectors[(c * stride) % vectors.size()];
            for (size_t d = 0; d < routing_dimensions; ++d)
                index->centers[c*routing_dimensions+d] = source[index->routing_columns[d]];
        }

        const size_t samples = std::min<size_t>(vectors.size(), 8192);
        std::vector<float> sums(index->centers.size());
        std::vector<uint32_t> counts(index->centroids);
        for (size_t iteration = 0; iteration < 4; ++iteration) {
            std::fill(sums.begin(), sums.end(), 0); std::fill(counts.begin(), counts.end(), 0);
            for (size_t s = 0; s < samples; ++s) {
                const float* vector = vectors[s * vectors.size() / samples];
                float best = std::numeric_limits<float>::infinity(); size_t selected = 0;
                for (size_t c = 0; c < index->centroids; ++c) {
                    const float distance = Distance(vector, index->centers.data()+c*routing_dimensions,
                                                    index->routing_columns, metric);
                    if (distance < best) { best = distance; selected = c; }
                }
                ++counts[selected];
                for (size_t d = 0; d < routing_dimensions; ++d)
                    sums[selected*routing_dimensions+d] += vector[index->routing_columns[d]];
            }
            for (size_t c = 0; c < index->centroids; ++c) if (counts[c])
                for (size_t d = 0; d < routing_dimensions; ++d)
                    index->centers[c*routing_dimensions+d] = sums[c*routing_dimensions+d] / counts[c];
        }

        std::vector<uint32_t> assignments(vectors.size());
        std::vector<float> assignment_distances(vectors.size());
        index->offsets.assign(index->centroids + 1, 0);
        Parallel(vectors.size(), [&](size_t row) {
            float best = std::numeric_limits<float>::infinity(); uint32_t selected = 0;
            for (uint32_t c = 0; c < index->centroids; ++c) {
                const float distance = Distance(vectors[row], index->centers.data()+c*routing_dimensions,
                                                index->routing_columns, metric);
                if (distance < best) { best = distance; selected = c; }
            }
            assignments[row] = selected;
            assignment_distances[row] = best;
        });
        index->mean_assignment_distance=std::accumulate(
            assignment_distances.begin(),assignment_distances.end(),0.0) / assignment_distances.size();
        for (uint32_t assignment : assignments) ++index->offsets[assignment+1];
        std::partial_sum(index->offsets.begin(), index->offsets.end(), index->offsets.begin());
        std::vector<uint32_t> positions = index->offsets;
        index->row_ids.resize(vectors.size());
        std::vector<const float*> ordered(vectors.size());
        for (size_t row = 0; row < vectors.size(); ++row) {
            const uint32_t position = positions[assignments[row]]++;
            index->row_ids[position] = global_ids[row]; ordered[position] = vectors[row];
        }

        // Per-dimension symmetric int8 sketches are list ordered for sequential shortlist scans.
        float sketch_maximum = 0;
        for (const float* vector : vectors) for (size_t d = 0; d < routing_dimensions; ++d)
            sketch_maximum = std::max(sketch_maximum, std::fabs(vector[index->routing_columns[d]]));
        index->sketch_scales.assign(routing_dimensions,
            sketch_maximum > 0 ? 127.0F / sketch_maximum : 1.0F);
        index->sketches.resize(vectors.size() * routing_dimensions);
        index->packed_vectors.resize(vectors.size() * dimension);
        index->inverse_norms.resize(vectors.size()); index->norm_squared.resize(vectors.size());
        Parallel(vectors.size(), [&](size_t row) {
            std::copy_n(ordered[row], dimension, index->packed_vectors.data()+row*dimension);
            const float norm=VectorDotProduct(ordered[row],ordered[row],dimension);
            index->norm_squared[row]=norm;
            index->inverse_norms[row]=norm>0?1.0F/std::sqrt(norm):0.0F;
            for (size_t d = 0; d < routing_dimensions; ++d)
                index->sketches[row*routing_dimensions+d] = int8_t(std::clamp(
                    std::lrint(ordered[row][index->routing_columns[d]] * index->sketch_scales[d]), -127L, 127L));
        });

        // Safe list pruning bounds for every numeric predicate field.
        index->bounds.resize(dataset.Fields().size());
        for (size_t f = 0; f < dataset.Fields().size(); ++f) {
            const auto type = dataset.Fields()[f].Type();
            if (type != FieldType::kInt32 && type != FieldType::kInt64 && type != FieldType::kFloat64 &&
                type != FieldType::kBool && type != FieldType::kDictInt32) continue;
            auto& bound = index->bounds[f]; bound.numeric = true;
            bound.minimum.assign(index->centroids, std::numeric_limits<double>::infinity());
            bound.maximum.assign(index->centroids, -std::numeric_limits<double>::infinity());
        }
        global = 0; size_t source_cursor = 0;
        for (const auto& segment : dataset.Segments()) {
            for (size_t row = 0; row < segment.RowCount(); ++row, ++global) {
                if (source_cursor >= global_ids.size() || global_ids[source_cursor] != global) continue;
                const uint32_t c = assignments[source_cursor++];
                for (size_t f = 0; f < index->bounds.size(); ++f) if (index->bounds[f].numeric) {
                    double value = 0; if (!Numeric(segment, f, row, &value)) continue;
                    index->bounds[f].minimum[c] = std::min(index->bounds[f].minimum[c], value);
                    index->bounds[f].maximum[c] = std::max(index->bounds[f].maximum[c], value);
                }
            }
        }
        index->build_seconds = std::chrono::duration<double>(Clock::now()-started).count();
        { std::lock_guard lock(mutex_); indexes_[key] = std::move(index); }
        return true;
    }

    std::shared_ptr<const Index> Get(const Dataset& dataset, size_t field, VectorMetric metric) {
        if (!Build(dataset, field, metric)) return {};
        std::lock_guard lock(mutex_); auto found = indexes_.find({&dataset,field,metric});
        return found == indexes_.end() ? nullptr : found->second;
    }
    bool Ready(const Dataset& dataset,size_t field,VectorMetric metric) {
        size_t rows=0;for(const auto& segment:dataset.Segments())rows+=segment.RowCount();
        std::lock_guard lock(mutex_);auto found=indexes_.find({&dataset,field,metric});
        return found!=indexes_.end()&&found->second->source_rows==rows&&
               found->second->segments==dataset.Segments().size()&&
               found->second->dimension==dataset.VectorDimension(field);
    }
    bool Save(const Dataset& dataset,size_t field,VectorMetric metric,const char* path) {
        std::shared_ptr<const Index> index;
        {
            std::lock_guard lock(mutex_);
            auto found=indexes_.find({&dataset,field,metric});
            if(found==indexes_.end())return false;
            index=found->second;
        }
        std::vector<uint8_t> payload;
        auto pod=[&](const auto& value){const auto* p=reinterpret_cast<const uint8_t*>(&value);payload.insert(payload.end(),p,p+sizeof(value));};
        auto vector=[&](const auto& values){uint64_t count=values.size();pod(count);const auto* p=reinterpret_cast<const uint8_t*>(values.data());payload.insert(payload.end(),p,p+values.size()*sizeof(values[0]));};
        pod(index->rows);pod(index->source_rows);pod(index->segments);pod(index->dimension);
        pod(index->routing_dimensions);pod(index->centroids);pod(index->builds);
        pod(index->build_seconds);pod(index->mean_assignment_distance);
        vector(index->routing_columns);vector(index->offsets);vector(index->row_ids);
        vector(index->centers);vector(index->sketch_scales);vector(index->packed_vectors);
        vector(index->inverse_norms);vector(index->norm_squared);vector(index->sketches);
        uint64_t bounds=index->bounds.size();pod(bounds);
        for(const auto& bound:index->bounds){uint8_t numeric=bound.numeric;pod(numeric);vector(bound.minimum);vector(bound.maximum);}
        uint64_t checksum=1469598103934665603ULL;
        for(uint8_t byte:payload){checksum^=byte;checksum*=1099511628211ULL;}
        struct Header{uint32_t magic,version;uint64_t schema,rows,payload_bytes,checksum;uint32_t field,metric;} header{
            0x4D495646U,1,dataset.SchemaFingerprint(),index->source_rows,payload.size(),checksum,
            static_cast<uint32_t>(field),static_cast<uint32_t>(metric)};
        const std::filesystem::path target(path),temporary=target.string()+".tmp";
        std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&header),sizeof(header));
        out.write(reinterpret_cast<const char*>(payload.data()),payload.size());out.close();
        if(!out.good())return false;
        std::error_code ec;std::filesystem::rename(temporary,target,ec);
        if(ec){std::filesystem::remove(temporary);return false;}return true;
    }
    bool Load(const Dataset& dataset,size_t field,VectorMetric metric,const char* path) {
        struct Header{uint32_t magic,version;uint64_t schema,rows,payload_bytes,checksum;uint32_t field,metric;} header{};
        std::ifstream in(path,std::ios::binary);in.read(reinterpret_cast<char*>(&header),sizeof(header));
        size_t rows=0;for(const auto& segment:dataset.Segments())rows+=segment.RowCount();
        if(!in.good()||header.magic!=0x4D495646U||header.version!=1||header.schema!=dataset.SchemaFingerprint()||
           header.rows!=rows||header.field!=field||header.metric!=static_cast<uint32_t>(metric)||header.payload_bytes>(1ULL<<40))return false;
        std::vector<uint8_t> payload(header.payload_bytes);in.read(reinterpret_cast<char*>(payload.data()),payload.size());
        if(!in.good())return false;
        uint64_t checksum=1469598103934665603ULL;
        for(uint8_t byte:payload){checksum^=byte;checksum*=1099511628211ULL;}if(checksum!=header.checksum)return false;
        size_t cursor=0;auto pod=[&](auto* value){if(cursor>payload.size()||sizeof(*value)>payload.size()-cursor)return false;std::memcpy(value,payload.data()+cursor,sizeof(*value));cursor+=sizeof(*value);return true;};
        auto vector=[&](auto* values){uint64_t count=0;if(!pod(&count)||count>SIZE_MAX/sizeof((*values)[0])||count*sizeof((*values)[0])>payload.size()-cursor)return false;values->resize(count);std::memcpy(values->data(),payload.data()+cursor,count*sizeof((*values)[0]));cursor+=count*sizeof((*values)[0]);return true;};
        auto index=std::make_shared<Index>();
        if(!pod(&index->rows)||!pod(&index->source_rows)||!pod(&index->segments)||!pod(&index->dimension)||
           !pod(&index->routing_dimensions)||!pod(&index->centroids)||!pod(&index->builds)||
           !pod(&index->build_seconds)||!pod(&index->mean_assignment_distance)||
           !vector(&index->routing_columns)||!vector(&index->offsets)||!vector(&index->row_ids)||
           !vector(&index->centers)||!vector(&index->sketch_scales)||!vector(&index->packed_vectors)||
           !vector(&index->inverse_norms)||!vector(&index->norm_squared)||!vector(&index->sketches))return false;
        uint64_t bounds=0;if(!pod(&bounds)||bounds>dataset.Fields().size())return false;index->bounds.resize(bounds);
        for(auto& bound:index->bounds){uint8_t numeric=0;if(!pod(&numeric)||!vector(&bound.minimum)||!vector(&bound.maximum))return false;bound.numeric=numeric!=0;}
        if(cursor!=payload.size()||index->source_rows!=rows||index->dimension!=dataset.VectorDimension(field)||
           index->offsets.size()!=index->centroids+1||index->row_ids.size()!=index->rows||
           index->packed_vectors.size()!=index->rows*index->dimension)return false;
        {std::lock_guard lock(mutex_);indexes_[{&dataset,field,metric}]=std::move(index);}return true;
    }
    std::shared_ptr<const ActiveIndex> GetActive(const Dataset& dataset,size_t field,
                                                 VectorMetric metric,const Index& index) {
        const Key key{&dataset,field,metric}; const size_t rows=dataset.ActiveRowCount();
        {
            std::lock_guard lock(mutex_); auto found=active_.find(key);
            if(found!=active_.end()&&found->second->rows==rows&&found->second->dimension==index.dimension)
                return found->second;
        }
        auto next=std::make_shared<ActiveIndex>(); next->rows=rows; next->dimension=index.dimension;
        next->offsets.assign(index.centroids+1,0);
        std::vector<uint32_t> assignments(rows);
        for(size_t row=0;row<rows;++row){
            size_t stored=0; const float* vector=dataset.ActiveFields()[field].VectorFloat32(row,&stored);
            if(!vector||stored!=index.dimension){assignments[row]=UINT32_MAX;continue;}
            float best=std::numeric_limits<float>::infinity();uint32_t selected=0;
            for(uint32_t c=0;c<index.centroids;++c){const float distance=Distance(vector,index.centers.data()+c*index.routing_dimensions,index.routing_columns,metric);
                if(distance<best){best=distance;selected=c;}}
            assignments[row]=selected;++next->offsets[selected+1];
        }
        std::partial_sum(next->offsets.begin(),next->offsets.end(),next->offsets.begin());
        next->positions.resize(next->offsets.back()); next->vectors.resize(next->positions.size()*index.dimension);
        next->inverse_norms.resize(next->positions.size());next->norm_squared.resize(next->positions.size());
        std::vector<uint32_t> write=next->offsets;
        for(uint32_t row=0;row<rows;++row)if(assignments[row]!=UINT32_MAX){
            const uint32_t position=write[assignments[row]]++;next->positions[position]=row;
            size_t stored=0;const float* vector=dataset.ActiveFields()[field].VectorFloat32(row,&stored);
            std::copy_n(vector,index.dimension,next->vectors.data()+size_t(position)*index.dimension);
            const float norm=VectorDotProduct(vector,vector,index.dimension);next->norm_squared[position]=norm;
            next->inverse_norms[position]=norm>0?1.0F/std::sqrt(norm):0.0F;
        }
        {std::lock_guard lock(mutex_);active_[key]=next;} return next;
    }
    IvfSearchStats Stats(const Dataset& dataset, size_t field, VectorMetric metric) {
        std::lock_guard lock(mutex_); auto found = indexes_.find({&dataset,field,metric});
        if (found == indexes_.end()) return {};
        IvfSearchStats result; result.indexed_rows=found->second->rows;
        result.centroid_count=found->second->centroids; result.routing_dimensions=found->second->routing_dimensions;
        result.builds=found->second->builds; result.build_seconds=found->second->build_seconds;
        result.mean_assignment_distance=found->second->mean_assignment_distance; return result;
    }
    void Release(const Dataset& dataset) {
        std::lock_guard lock(mutex_);
        for (auto it=indexes_.begin(); it!=indexes_.end();) it = it->first.dataset==&dataset ? indexes_.erase(it) : std::next(it);
        for (auto it=active_.begin(); it!=active_.end();) it = it->first.dataset==&dataset ? active_.erase(it) : std::next(it);
    }
private:
    std::mutex mutex_;
    std::unordered_map<Key,std::shared_ptr<const Index>,KeyHash> indexes_;
    std::unordered_map<Key,std::shared_ptr<const ActiveIndex>,KeyHash> active_;
};

bool ListMayMatch(const Index& index, uint32_t center,
                  const std::vector<VectorSearchPredicate>& predicates) {
    for (const auto& predicate : predicates) {
        if (predicate.field_index >= index.bounds.size()) return false;
        const auto& bound = index.bounds[predicate.field_index];
        if (!bound.numeric || !std::isfinite(bound.minimum[center])) continue;
        if (!MayMatch(bound.minimum[center], bound.maximum[center], predicate.op, predicate.value)) return false;
    }
    return true;
}
std::mutex last_stats_mutex;
IvfSearchStats last_stats;
Key last_key{};
} // namespace

bool BuildVectorIvf(const Dataset& dataset, size_t field, VectorMetric metric) {
    auto& store=Store::Instance();
    if(!store.Build(dataset,field,metric)) return false;
    const auto index=store.Get(dataset,field,metric);
    if(index&&dataset.ActiveRowCount()) store.GetActive(dataset,field,metric,*index);
    return true;
}
bool VectorIvfReady(const Dataset& dataset,size_t field,VectorMetric metric){
    return Store::Instance().Ready(dataset,field,metric);
}
bool SaveVectorIvf(const Dataset& dataset,size_t field,VectorMetric metric,const char* path){
    return path&&Store::Instance().Save(dataset,field,metric,path);
}
bool LoadVectorIvf(const Dataset& dataset,size_t field,VectorMetric metric,const char* path){
    return path&&Store::Instance().Load(dataset,field,metric,path);
}

bool VectorSearchIvf(const Dataset& dataset, size_t field, const float* query,
                     size_t dimension, size_t top_k, VectorMetric metric, size_t probes,
                     std::vector<VectorSearchHit>* out,
                     const std::vector<VectorSearchPredicate>& predicates) {
    const auto index = Store::Instance().Get(dataset,field,metric);
    if (!index) {
        size_t sealed=0; for (const auto& segment:dataset.Segments()) sealed+=segment.RowCount();
        if (sealed) return false;
        return VectorSearchCandidates(dataset,field,query,dimension,top_k,metric,{},out,predicates);
    }
    if (!query || dimension != index->dimension || !out || !top_k) return false;
    double maximum_assignment_distance=0.35;
    if(const char* value=std::getenv("MIMICDB_IVF_MAX_ASSIGNMENT_DISTANCE")){
        const double parsed=std::strtod(value,nullptr);if(parsed>0)maximum_assignment_distance=parsed;
    }
    if(probes==0 && metric==VectorMetric::kCosine &&
       index->mean_assignment_distance>maximum_assignment_distance) {
        const auto started=Clock::now();
        const bool ok=VectorSearch(dataset,field,query,dimension,top_k,metric,out,predicates);
        std::lock_guard lock(last_stats_mutex); last_stats={};
        last_stats.indexed_rows=index->rows;last_stats.centroid_count=index->centroids;
        last_stats.routing_dimensions=index->routing_dimensions;last_stats.builds=index->builds;
        last_stats.build_seconds=index->build_seconds;
        last_stats.mean_assignment_distance=index->mean_assignment_distance;
        last_stats.exact_fallback=true;last_stats.rerank_seconds=std::chrono::duration<double>(Clock::now()-started).count();
        last_key={&dataset,field,metric};return ok;
    }
    const auto route_start=Clock::now();
    std::vector<std::pair<float,uint32_t>> ranked;
    ranked.reserve(index->centroids); size_t pruned=0;
    for (uint32_t c=0;c<index->centroids;++c) {
        if (!ListMayMatch(*index,c,predicates)) { ++pruned; continue; }
        ranked.push_back({Distance(query,index->centers.data()+c*index->routing_dimensions,
                                   index->routing_columns,metric),c});
    }
    std::sort(ranked.begin(),ranked.end());
    if (probes==0) {
        probes=std::max<size_t>(4,size_t(std::sqrt(double(index->centroids))));
        // Easy queries have a pronounced routing gap and need fewer lists.
        if (ranked.size()>4 && ranked[4].first > ranked[0].first*1.8F) probes=4;
    }
    probes=std::min(probes,ranked.size());
    const double routing_seconds=std::chrono::duration<double>(Clock::now()-route_start).count();

    std::vector<uint32_t> positions;
    size_t candidates=0;
    for(size_t p=0;p<probes;++p) candidates+=index->offsets[ranked[p].second+1]-index->offsets[ranked[p].second];
    positions.reserve(candidates);
    for(size_t p=0;p<probes;++p) for(uint32_t pos=index->offsets[ranked[p].second];pos<index->offsets[ranked[p].second+1];++pos) positions.push_back(pos);
    std::shared_ptr<const ActiveIndex> active;
    std::vector<uint32_t> active_positions;
    if(predicates.empty()&&dataset.ActiveRowCount()){
        active=Store::Instance().GetActive(dataset,field,metric,*index);
        for(size_t p=0;p<probes;++p){const uint32_t center=ranked[p].second;
            for(uint32_t pos=active->offsets[center];pos<active->offsets[center+1];++pos)
                active_positions.push_back(pos);
        }
    }

    const auto shortlist_start=Clock::now();
    const double routing_confidence=ranked.size()>1
        ? std::max(0.0,double(ranked[1].first-ranked[0].first)) /
          std::max(1e-3,std::fabs(double(ranked[0].first))) : 1.0;
    const size_t base_shortlist=std::max<size_t>(top_k*32,1024);
    const size_t probe_factor=std::max<size_t>(1,(probes*4+index->centroids-1)/index->centroids);
    size_t shortlist_limit=std::max({base_shortlist*probe_factor,
                                     (positions.size()+15)/16,base_shortlist});
    if(routing_confidence<0.25) shortlist_limit=std::max(shortlist_limit,base_shortlist*4);
    if (const char* value=std::getenv("MIMICDB_IVF_SHORTLIST")) { const size_t parsed=std::strtoull(value,nullptr,10); if(parsed) shortlist_limit=std::max(parsed,top_k); }
    shortlist_limit=std::min(shortlist_limit,positions.size());
    // Predicate paths retain all row IDs: the existing scorer intersects predicates before full vectors.
    const bool use_shortlist=predicates.empty() && positions.size()>16384 &&
                             positions.size()>shortlist_limit;
    if (use_shortlist) {
        std::vector<int16_t> quantized(index->routing_dimensions);
        for(size_t d=0;d<index->routing_dimensions;++d) quantized[d]=int16_t(std::clamp(
            std::lrint(query[index->routing_columns[d]]*index->sketch_scales[d]),-127L,127L));
        std::vector<std::pair<float,uint32_t>> scores(positions.size());
        const size_t score_workers=std::min(positions.size(),GetVectorSearchRuntimeStats().cpu_threads);
        RunVectorParallel(score_workers,[&](size_t worker){
            const size_t first=worker*positions.size()/score_workers;
            const size_t last=(worker+1)*positions.size()/score_workers;
            for(size_t i=first;i<last;++i){
                const int8_t* sketch=index->sketches.data()+positions[i]*index->routing_dimensions;
                int64_t dot=0, left_norm=0, right_norm=0, l2=0;
                for(size_t d=0;d<index->routing_dimensions;++d) {
                    const int left=quantized[d], right=sketch[d];
                    if(metric==VectorMetric::kL2Squared){ const int delta=left-right; l2+=delta*delta; }
                    else { dot+=left*right; if(metric==VectorMetric::kCosine){left_norm+=left*left;right_norm+=right*right;} }
                }
                float score=metric==VectorMetric::kL2Squared?float(l2):float(-dot);
                if(metric==VectorMetric::kCosine) score=left_norm<=0||right_norm<=0?1.0F:
                    1.0F-float(dot)/std::sqrt(float(left_norm)*float(right_norm));
                scores[i]={score,positions[i]};
            }
        });
        std::nth_element(scores.begin(),scores.begin()+shortlist_limit,scores.end());
        positions.resize(shortlist_limit);
        for(size_t i=0;i<shortlist_limit;++i) positions[i]=scores[i].second;
    }
    std::vector<uint32_t> rows; rows.reserve(positions.size());
    for(uint32_t position:positions) rows.push_back(index->row_ids[position]);
    const double shortlist_seconds=std::chrono::duration<double>(Clock::now()-shortlist_start).count();
    const auto rerank_start=Clock::now();
    bool ok=false;
    if(predicates.empty()) {
        struct Worse { bool operator()(const VectorSearchHit& a,const VectorSearchHit& b) const {
            return a.distance!=b.distance?a.distance<b.distance:a.row_id<b.row_id; } };
        std::priority_queue<VectorSearchHit,std::vector<VectorSearchHit>,Worse> heap;
        const float query_norm_squared=VectorDotProduct(query,query,dimension);
        const float query_inverse_norm=query_norm_squared>0?1.0F/std::sqrt(query_norm_squared):0.0F;
        const size_t total_positions=positions.size()+active_positions.size();
        const size_t workers=std::min(total_positions,GetVectorSearchRuntimeStats().cpu_threads);
        std::vector<std::vector<VectorSearchHit>> partial(workers);
        RunVectorParallel(workers,[&](size_t worker){
            std::priority_queue<VectorSearchHit,std::vector<VectorSearchHit>,Worse> local;
            const size_t first=worker*total_positions/workers,last=(worker+1)*total_positions/workers;
            for(size_t i=first;i<last;++i){
                const bool sealed=i<positions.size();
                const uint32_t position=sealed?positions[i]:active_positions[i-positions.size()];
                const float* vector=sealed?index->packed_vectors.data()+size_t(position)*dimension:
                    active->vectors.data()+size_t(position)*dimension;
                const float inverse_norm=sealed?index->inverse_norms[position]:active->inverse_norms[position];
                const float stored_norm=sealed?index->norm_squared[position]:active->norm_squared[position];
                const float dot=VectorDotProduct(vector,query,dimension);
                float distance=-dot;
                if(metric==VectorMetric::kCosine) distance=inverse_norm<=0||query_inverse_norm<=0?1.0F:
                    1.0F-dot*inverse_norm*query_inverse_norm;
                else if(metric==VectorMetric::kL2Squared) distance=stored_norm+query_norm_squared-2.0F*dot;
                const uint64_t row_id=sealed?index->row_ids[position]:index->source_rows+active->positions[position];
                const VectorSearchHit hit{row_id,distance};
                if(local.size()<top_k)local.push(hit);
                else if(hit.distance<local.top().distance||(hit.distance==local.top().distance&&hit.row_id<local.top().row_id)){local.pop();local.push(hit);}
            }
            while(!local.empty()){partial[worker].push_back(local.top());local.pop();}
        });
        for(const auto& part:partial) for(const auto& hit:part){
            if(heap.size()<top_k)heap.push(hit);
            else if(hit.distance<heap.top().distance||(hit.distance==heap.top().distance&&hit.row_id<heap.top().row_id)){heap.pop();heap.push(hit);}
        }
        ok=true;
        out->resize(heap.size()); for(size_t i=heap.size();i>0;--i){(*out)[i-1]=heap.top();heap.pop();}
        std::sort(out->begin(),out->end(),[](auto a,auto b){return a.distance!=b.distance?a.distance<b.distance:a.row_id<b.row_id;});
    } else ok=VectorSearchCandidates(dataset,field,query,dimension,top_k,metric,rows,out,predicates);
    const double rerank_seconds=std::chrono::duration<double>(Clock::now()-rerank_start).count();
    {
        std::lock_guard lock(last_stats_mutex);
        last_stats={}; last_stats.indexed_rows=index->rows; last_stats.centroid_count=index->centroids;
        last_stats.probes=probes; last_stats.candidates=candidates; last_stats.routing_dimensions=index->routing_dimensions;
        last_stats.builds=index->builds; last_stats.shortlisted=rows.size(); last_stats.lists_pruned=pruned;
        last_stats.shortlist_limit=shortlist_limit; last_stats.routing_confidence=routing_confidence;
        last_stats.routing_seconds=routing_seconds; last_stats.shortlist_seconds=shortlist_seconds;
        last_stats.rerank_seconds=rerank_seconds; last_stats.build_seconds=index->build_seconds;
        last_stats.mean_assignment_distance=index->mean_assignment_distance;
        last_key={&dataset,field,metric};
    }
    return ok;
}

IvfSearchStats GetVectorIvfStats(const Dataset& dataset,size_t field,VectorMetric metric) {
    std::lock_guard lock(last_stats_mutex);
    if(last_key==Key{&dataset,field,metric}) return last_stats;
    return Store::Instance().Stats(dataset,field,metric);
}
void ReleaseVectorIvfDataset(const Dataset& dataset){ Store::Instance().Release(dataset); }
} // namespace mimicdb
