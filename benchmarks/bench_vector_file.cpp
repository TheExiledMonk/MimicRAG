#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "mimicdb/dataset.h"
#include "mimicdb/vector_ivf.h"
#include "mimicdb/vector_search.h"

namespace {
bool ReadFvecs(const std::string& path,size_t limit,std::vector<float>* values,size_t* dimension) {
    std::ifstream in(path,std::ios::binary);if(!in)return false;
    values->clear();*dimension=0;
    while(in&&(!limit||values->size()/std::max<size_t>(1,*dimension)<limit)){
        int32_t dim=0;in.read(reinterpret_cast<char*>(&dim),sizeof(dim));if(!in)break;
        if(dim<=0||(*dimension&&size_t(dim)!=*dimension))return false;
        if(!*dimension)*dimension=dim;const size_t offset=values->size();values->resize(offset+dim);
        in.read(reinterpret_cast<char*>(values->data()+offset),size_t(dim)*sizeof(float));if(!in)return false;
    }
    return *dimension&& !values->empty();
}
bool ReadIvecs(const std::string& path,size_t limit,std::vector<std::vector<uint32_t>>* rows) {
    std::ifstream in(path,std::ios::binary);if(!in)return false;rows->clear();
    while(in&&(!limit||rows->size()<limit)){
        int32_t count=0;in.read(reinterpret_cast<char*>(&count),sizeof(count));if(!in)break;
        if(count<=0)return false;std::vector<int32_t> raw(count);
        in.read(reinterpret_cast<char*>(raw.data()),size_t(count)*sizeof(int32_t));if(!in)return false;
        rows->emplace_back();for(int32_t value:raw)if(value>=0)rows->back().push_back(uint32_t(value));
    }
    return !rows->empty();
}
double Percentile(std::vector<double> values,double fraction){
    std::sort(values.begin(),values.end());return values[std::min(values.size()-1,size_t(values.size()*fraction))];
}
}

int main(int argc,char** argv){
    if(argc<3){std::cerr<<"usage: mimicdb_bench_vector_file BASE.fvecs QUERY.fvecs [GROUNDTRUTH.ivecs] [base_limit] [query_limit] [probes]\n";return 2;}
    const size_t base_limit=argc>4?std::strtoull(argv[4],nullptr,10):0;
    const size_t query_limit=argc>5?std::strtoull(argv[5],nullptr,10):1000;
    const size_t probes=argc>6?std::strtoull(argv[6],nullptr,10):0;
    std::vector<float> base,queries;size_t dimension=0,query_dimension=0;
    if(!ReadFvecs(argv[1],base_limit,&base,&dimension)||
       !ReadFvecs(argv[2],query_limit,&queries,&query_dimension)||dimension!=query_dimension)return 2;
    std::vector<std::vector<uint32_t>> truth;
    if(argc>3&&!ReadIvecs(argv[3],queries.size()/dimension,&truth))return 2;
    mimicdb::Dataset dataset("public_vectors");
    dataset.AddField(mimicdb::FieldVector("embedding",mimicdb::FieldType::kVectorFloat32));
    const size_t rows=base.size()/dimension,query_count=queries.size()/dimension;
    for(size_t row=0;row<rows;++row)if(!dataset.Append({mimicdb::FieldValue::VectorFloat32(
        std::vector<float>(base.begin()+row*dimension,base.begin()+(row+1)*dimension))}))return 2;
    const auto build_start=std::chrono::steady_clock::now();
    if(!mimicdb::BuildVectorIvf(dataset,0,mimicdb::VectorMetric::kCosine))return 2;
    const double build_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-build_start).count();
    std::vector<double> timings;double recall_sum=0,mrr_sum=0;size_t evaluated=0;
    for(size_t q=0;q<query_count;++q){
        const float* query=queries.data()+q*dimension;std::vector<mimicdb::VectorSearchHit> actual,exact;
        const auto start=std::chrono::steady_clock::now();
        if(!mimicdb::VectorSearchIvf(dataset,0,query,dimension,10,mimicdb::VectorMetric::kCosine,probes,&actual))return 2;
        timings.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
        std::vector<uint32_t> expected;
        if(q<truth.size())expected.assign(truth[q].begin(),truth[q].begin()+std::min<size_t>(10,truth[q].size()));
        else {if(!mimicdb::VectorSearch(dataset,0,query,dimension,10,mimicdb::VectorMetric::kCosine,&exact))return 2;
              for(const auto& hit:exact)expected.push_back(uint32_t(hit.row_id));}
        size_t overlap=0;double reciprocal=0;
        for(size_t rank=0;rank<actual.size();++rank)if(std::find(expected.begin(),expected.end(),actual[rank].row_id)!=expected.end()){
            ++overlap;if(reciprocal==0)reciprocal=1.0/(rank+1);}
        recall_sum+=expected.empty()?1.0:double(overlap)/expected.size();mrr_sum+=reciprocal;++evaluated;
    }
    const auto stats=mimicdb::GetVectorIvfStats(dataset,0,mimicdb::VectorMetric::kCosine);
    std::cout<<"benchmark=vector_public rows="<<rows<<" dimension="<<dimension<<" queries="<<evaluated
             <<" probes="<<stats.probes<<" p50_seconds="<<Percentile(timings,0.5)
             <<" p95_seconds="<<Percentile(timings,0.95)<<" p99_seconds="<<Percentile(timings,0.99)
             <<" recall_at_10="<<recall_sum/evaluated<<" mrr="<<mrr_sum/evaluated
             <<" build_seconds="<<build_seconds<<" exact_fallback="<<stats.exact_fallback<<"\n";
    return 0;
}
