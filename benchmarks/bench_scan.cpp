#include <chrono>
#include <cstddef>
#include <iostream>
#include <vector>

#include "pcdb/scan.h"
#include "pcdb/metrics.h"

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

int main() {
    constexpr size_t kCount = 1 << 20;
    std::vector<int> data(kCount, 1);
    PredCtx pred{data.data(), 0};
    CountCtx out;
    pcdb::Metrics metrics;

    auto start = std::chrono::high_resolution_clock::now();
    pcdb::ScanLoopWithMetrics(kCount, Predicate, &pred, Consume, &out, &metrics, sizeof(int));
    auto end = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<double> elapsed = end - start;

    const double rows_per_sec = static_cast<double>(metrics.rows) / elapsed.count();
    const double bytes_per_sec = static_cast<double>(metrics.bytes) / elapsed.count();
    std::cout << "rows=" << kCount << " kept=" << out.count
              << " seconds=" << elapsed.count()
              << " rows_per_sec=" << rows_per_sec
              << " bytes_per_sec=" << bytes_per_sec << "\n";
    return 0;
}
