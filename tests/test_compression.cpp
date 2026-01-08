#include <cassert>

#include "mimicdb/aggregate.h"
#include "mimicdb/compression.h"
#include "mimicdb/field_vector.h"
#include "mimicdb/mask.h"
#include "mimicdb/scan.h"

int main() {
    mimicdb::FieldVector field("age", mimicdb::FieldType::kInt64);
    field.AppendInt64(10);
    field.AppendInt64(20);
    field.AppendInt64(30);
    field.AppendInt64(40);

    mimicdb::CompressedColumnView view = mimicdb::MakeUncompressedView(field);
    assert(view.kind == mimicdb::ColumnCompressionKind::kNone);
    assert(mimicdb::MemoryBytes(view) == field.Size() * sizeof(int64_t));

    mimicdb::CompressionPredicate predicate;
    predicate.type = mimicdb::FieldType::kInt64;
    predicate.op = mimicdb::CompareOp::kGt;
    predicate.i64 = 15;

    mimicdb::Mask mask;
    const bool built = mimicdb::BuildMaskCompressed(view, predicate, &mask);
    assert(built);
    assert(mask.Size() == field.Size());
    assert(!mask.Get(0));
    assert(mask.Get(1));
    assert(mask.Get(2));
    assert(mask.Get(3));

    mimicdb::AggregateResult result;
    mimicdb::AggregateCompressed(view, &mask, &result);
    assert(result.count == 3);
    assert(result.sum == 90.0);
    assert(result.min == 20.0);
    assert(result.max == 40.0);

    return 0;
}
