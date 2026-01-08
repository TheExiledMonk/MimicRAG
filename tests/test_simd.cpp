#include <cassert>
#include <cstddef>
#include <vector>

#include "mimicdb/mask.h"
#include "mimicdb/simd.h"

int main() {
    const int64_t values[] = {1, 2, 2, 5, 2};
    std::vector<uint8_t> out(5, 0);

    auto pred = mimicdb::GetPredicateKernelInt64Eq();
    assert(pred != nullptr);
    pred(values, 2, out.data(), out.size());
    assert(out[0] == 0);
    assert(out[1] == 1);
    assert(out[2] == 1);
    assert(out[3] == 0);
    assert(out[4] == 1);

    const double fvalues[] = {1.0, 2.0, 3.0};
    mimicdb::AggregateResult result;
    auto agg = mimicdb::GetAggregateKernelDouble();
    assert(agg != nullptr);
    agg(fvalues, 3, nullptr, &result);
    assert(result.count == 3);
    assert(result.sum == 6.0);

    return 0;
}
