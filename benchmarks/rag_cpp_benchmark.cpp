#include "mimicrag/rag_engine.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

namespace {
struct Topic { const char* document; const char* query; };
constexpr Topic kTopics[] = {
    {"relational database query optimizer and columnar storage execution", "fast analytical SQL engine"},
    {"marine whales coral reefs and ocean ecosystem conservation", "protecting sea life habitats"},
    {"hydraulic pump pressure valve maintenance for industrial machinery", "repair fluid power equipment"},
    {"solar photovoltaic panels battery inverter renewable electricity", "home energy from sunlight"},
    {"neural network training gradient descent and machine learning", "optimizing artificial intelligence models"},
    {"cardiac heart rhythm blood circulation and medical diagnosis", "clinical cardiovascular health"},
    {"distributed consensus replication fault tolerance and leader election", "reliable cluster coordination"},
    {"bread fermentation sourdough yeast flour and artisan baking", "making a naturally leavened loaf"},
    {"aircraft turbine aerodynamics navigation and aviation safety", "safe operation of passenger airplanes"},
    {"constitutional law judicial precedent civil rights and legislation", "legal protection by courts and statutes"},
    {"forest wildfire prevention ecology trees and habitat restoration", "recovering woodland after fires"},
    {"quantum particles wave functions measurement and theoretical physics", "probability in subatomic mechanics"},
};

double Milliseconds(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
double Percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<size_t>(fraction * (values.size() - 1)))];
}

struct QueryStats { std::vector<double> latency; size_t recall1 = 0, recall10 = 0, filter_errors = 0; };

void Merge(QueryStats& target, QueryStats source) {
    target.latency.insert(target.latency.end(), source.latency.begin(), source.latency.end());
    target.recall1 += source.recall1; target.recall10 += source.recall10; target.filter_errors += source.filter_errors;
}

QueryStats RunQueries(mimicrag::RagEngine& engine, size_t begin, size_t count) {
    QueryStats stats; stats.latency.reserve(count);
    for (size_t i = begin; i < begin + count; ++i) {
        const size_t topic = i % std::size(kTopics); const std::string tenant = i % 2 ? "tenant-b" : "tenant-a";
        const auto started = Clock::now();
        const auto result = engine.Retrieve({{"query", kTopics[topic].query}, {"tenant_id", tenant}, {"access_scope", "public"}, {"top_k", 10}});
        stats.latency.push_back(Milliseconds(started));
        const std::string expected = "bench://topic/" + std::to_string(topic) + "/";
        bool found = false; size_t rank = 0;
        for (const auto& hit : result["hits"]) {
            ++rank; const bool relevant = hit.at("source_uri").get<std::string>().rfind(expected, 0) == 0;
            if (relevant && !found) { found = true; if (rank == 1) ++stats.recall1; ++stats.recall10; }
            if (hit.value("tenant_id", "") != tenant || hit.value("access_scope", "") != "public") ++stats.filter_errors;
        }
    }
    return stats;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: mimicrag_benchmark MODEL_GGUF DATA_DIR [documents=600] [queries=120] [threads=8]\n"; return 2; }
    const size_t documents = argc > 3 ? std::stoull(argv[3]) : 600;
    const size_t queries = argc > 4 ? std::stoull(argv[4]) : 120;
    const size_t threads = argc > 5 ? std::stoull(argv[5]) : 8;
    mimicrag::Config config;
    config.chat.provider = "openai_compatible"; config.chat.model = "disabled"; config.chat.base_url = "http://127.0.0.1:1";
    config.embedding.provider = "local"; config.embedding.model = "local-gguf";
    config.local_embedding.enabled = true; config.local_embedding.eager_dual_index = true; config.local_embedding.model_path = argv[1];
    config.local_embedding.gpu_layers = -1; config.local_embedding.threads = 0; config.local_embedding.context_size = 2048;
    config.local_embedding.document_prefix = "search_document: "; config.local_embedding.query_prefix = "search_query: ";
    config.server.data_path = argv[2]; config.server.trace_path = std::string(argv[2]) + "/traces.jsonl"; config.server.top_k = 10;

    const auto total_started = Clock::now(); auto engine = std::make_unique<mimicrag::RagEngine>(config);
    const auto ingest_started = Clock::now();
    for (size_t i = 0; i < documents; ++i) {
        const size_t topic = i % std::size(kTopics); const std::string tenant = i % 2 ? "tenant-b" : "tenant-a";
        const std::string text = std::string(kTopics[topic].document) + ". Reference manual section " + std::to_string(i) +
            " contains operational details, examples, troubleshooting notes, safety guidance, and background information.";
        engine->Ingest({{"text", text}, {"source_uri", "bench://topic/" + std::to_string(topic) + "/" + std::to_string(i)},
            {"tenant_id", tenant}, {"metadata", {{"access_scope", i % 5 == 0 ? "private" : "public"}}}, {"background", false}});
    }
    const double ingest_ms = Milliseconds(ingest_started);
    RunQueries(*engine, 0, std::min<size_t>(24, queries));
    const auto sequential_started = Clock::now(); const auto sequential = RunQueries(*engine, 0, queries); const double sequential_ms = Milliseconds(sequential_started);

    const auto concurrent_started = Clock::now(); std::vector<std::thread> workers; std::vector<QueryStats> partial(threads);
    for (size_t thread = 0; thread < threads; ++thread) {
        const size_t first = queries * thread / threads, last = queries * (thread + 1) / threads;
        workers.emplace_back([&, thread, first, last] { partial[thread] = RunQueries(*engine, first, last - first); });
    }
    for (auto& worker : workers) worker.join();
    const double concurrent_ms = Milliseconds(concurrent_started); QueryStats concurrent;
    for (auto& value : partial) Merge(concurrent, std::move(value));

    engine.reset(); const auto restart_started = Clock::now(); engine = std::make_unique<mimicrag::RagEngine>(config); const double restart_ms = Milliseconds(restart_started);
    const auto cold_started = Clock::now(); const auto cold = RunQueries(*engine, 0, 1); const double cold_ms = Milliseconds(cold_started);
    const auto health = engine->Health();
    auto report = [&](const QueryStats& stats, double elapsed) { return Json{{"queries", stats.latency.size()}, {"qps", stats.latency.size() * 1000.0 / elapsed},
        {"latency_ms", {{"p50", Percentile(stats.latency, .50)}, {"p95", Percentile(stats.latency, .95)}, {"p99", Percentile(stats.latency, .99)}, {"mean", std::accumulate(stats.latency.begin(), stats.latency.end(), 0.0) / std::max<size_t>(1, stats.latency.size())}}},
        {"recall_at_1", double(stats.recall1) / std::max<size_t>(1, stats.latency.size())}, {"recall_at_10", double(stats.recall10) / std::max<size_t>(1, stats.latency.size())}, {"filter_errors", stats.filter_errors}}; };
    std::cout << Json{{"documents", documents}, {"model", argv[1]}, {"device", health["local_embedding_device"]},
        {"vector_rows", health["local_vector_rows"]}, {"ingestion", {{"elapsed_ms", ingest_ms}, {"documents_per_second", documents * 1000.0 / ingest_ms}}},
        {"sequential", report(sequential, sequential_ms)}, {"concurrent", {{"threads", threads}, {"result", report(concurrent, concurrent_ms)}}},
        {"restart", {{"replay_ms", restart_ms}, {"first_query_ms", cold_ms}, {"first_query_recall_at_10", cold.recall10}}},
        {"total_elapsed_ms", Milliseconds(total_started)}}.dump(2) << '\n';
}
