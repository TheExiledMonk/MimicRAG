#include <cassert>

#include "mimicdb/mask.h"
#include "mimicdb/predicate.h"

int main() {
    mimicdb::Mask mask(8);
    mask.Set(1, true);
    mask.Set(3, true);
    assert(mask.Size() == 8);
    assert(mask.Get(1));
    assert(!mask.Get(2));

    mimicdb::Mask other(8);
    other.Set(3, true);
    other.Set(4, true);

    mimicdb::Mask and_mask = mimicdb::Mask::And(mask, other);
    assert(and_mask.Get(3));
    assert(!and_mask.Get(1));

    mimicdb::Mask or_mask = mimicdb::Mask::Or(mask, other);
    assert(or_mask.Get(1));
    assert(or_mask.Get(3));
    assert(or_mask.Get(4));

    const int64_t values[] = {1, 5, 10, 3};
    mimicdb::Int64Predicate pred;
    pred.data = values;
    pred.value = 4;
    pred.compare = [](int64_t left, int64_t right) {
        return mimicdb::CompareInt64(left, right, mimicdb::CompareOp::kGt);
    };
    mimicdb::Mask generated = mimicdb::BuildMask(pred, 4);
    assert(!generated.Get(0));
    assert(generated.Get(1));
    assert(generated.Get(2));
    assert(!generated.Get(3));

    mimicdb::Int64PredicateBranchless branchless;
    branchless.data = values;
    branchless.value = 4;
    branchless.op = mimicdb::CompareOp::kGt;
    mimicdb::Mask branch_mask = mimicdb::BuildMaskBranchless(branchless, 4);
    assert(!branch_mask.Get(0));
    assert(branch_mask.Get(1));
    assert(branch_mask.Get(2));
    assert(!branch_mask.Get(3));

    mimicdb::PackedMask packed = mimicdb::PackMask(branch_mask);
    assert(packed.Size() == 4);
    assert(!packed.Get(0));
    assert(packed.Get(1));
    assert(packed.Get(2));
    assert(!packed.Get(3));

    return 0;
}
