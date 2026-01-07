#include <cassert>

#include "pcdb/mask.h"
#include "pcdb/predicate.h"

int main() {
    pcdb::Mask mask(8);
    mask.Set(1, true);
    mask.Set(3, true);
    assert(mask.Size() == 8);
    assert(mask.Get(1));
    assert(!mask.Get(2));

    pcdb::Mask other(8);
    other.Set(3, true);
    other.Set(4, true);

    pcdb::Mask and_mask = pcdb::Mask::And(mask, other);
    assert(and_mask.Get(3));
    assert(!and_mask.Get(1));

    pcdb::Mask or_mask = pcdb::Mask::Or(mask, other);
    assert(or_mask.Get(1));
    assert(or_mask.Get(3));
    assert(or_mask.Get(4));

    const int64_t values[] = {1, 5, 10, 3};
    pcdb::Int64Predicate pred;
    pred.data = values;
    pred.value = 4;
    pred.compare = [](int64_t left, int64_t right) {
        return pcdb::CompareInt64(left, right, pcdb::CompareOp::kGt);
    };
    pcdb::Mask generated = pcdb::BuildMask(pred, 4);
    assert(!generated.Get(0));
    assert(generated.Get(1));
    assert(generated.Get(2));
    assert(!generated.Get(3));

    pcdb::Int64PredicateBranchless branchless;
    branchless.data = values;
    branchless.value = 4;
    branchless.op = pcdb::CompareOp::kGt;
    pcdb::Mask branch_mask = pcdb::BuildMaskBranchless(branchless, 4);
    assert(!branch_mask.Get(0));
    assert(branch_mask.Get(1));
    assert(branch_mask.Get(2));
    assert(!branch_mask.Get(3));

    pcdb::PackedMask packed = pcdb::PackMask(branch_mask);
    assert(packed.Size() == 4);
    assert(!packed.Get(0));
    assert(packed.Get(1));
    assert(packed.Get(2));
    assert(!packed.Get(3));

    return 0;
}
