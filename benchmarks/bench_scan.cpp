#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "mimicdb/scan.h"
#include "mimicdb/metrics.h"

namespace {
struct PredCtx {
    const int* data;
    int threshold;
};

bool Predicate(size_t index, void* ctx) {
    const auto* pred = static_cast<PredCtx*>(ctx);
    return pred->data[index] > pred->threshold;
}

struct CountCtx {
    size_t count = 0;
};

void Consume(size_t /*index*/, void* ctx) {
    auto* count = static_cast<CountCtx*>(ctx);
    ++count->count;
}
}  // namespace

size_t ParseArg(const char* arg, const char* name, size_t fallback) {
    const size_t len = std::strlen(name);
    if (std::strncmp(arg, name, len) != 0) {
        return fallback;
    }
    const char* value = arg + len;
    if (*value == '=') {
        value += 1;
    }
    const unsigned long long parsed = std::strtoull(value, nullptr, 10);
    return parsed == 0 ? fallback : static_cast<size_t>(parsed);
}

int main(int argc, char** argv) {
    size_t count = 1 << 20;
    for (int i = 1; i < argc; ++i) {
        count = ParseArg(argv[i], "--rows", count);
        count = ParseArg(argv[i], "--count", count);
    }
    std::vector<int> data(count, 1);
    PredCtx pred{data.data(), 0};
    CountCtx out;
    mimicdb::Metrics metrics;

    auto start = std::chrono::high_resolution_clock::now();
    mimicdb::ScanLoopWithMetrics(count, Predicate, &pred, Consume, &out, &metrics, sizeof(int));
    auto end = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<double> elapsed = end - start;

    const double rows_per_sec = static_cast<double>(metrics.rows) / elapsed.count();
    const double bytes_per_sec = static_cast<double>(metrics.bytes) / elapsed.count();
    std::cout << "benchmark=scan"
              << " rows=" << count
              << " kept=" << out.count
              << " seconds=" << elapsed.count()
              << " rows_per_sec=" << rows_per_sec
              << " bytes_per_sec=" << bytes_per_sec
              << " branch_misses=" << metrics.branch_misses
              << " cache_misses=" << metrics.cache_misses
              << "\n";
    return 0;
}
