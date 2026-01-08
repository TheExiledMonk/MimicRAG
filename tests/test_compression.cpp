#include <cassert>

#include "mimicdb/aggregate.h"
#include "mimicdb/compression.h"
#include "mimicdb/field_vector.h"
#include "mimicdb/mask.h"
#include "mimicdb/segment.h"
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

    mimicdb::FieldVector dict_field("dict", mimicdb::FieldType::kInt64);
    dict_field.AppendInt64(10);
    dict_field.AppendInt64(20);
    dict_field.AppendInt64(10);
    dict_field.AppendInt64(30);
    std::vector<mimicdb::FieldVector> dict_fields;
    dict_fields.push_back(dict_field);
    mimicdb::Segment dict_segment(4, 4, std::move(dict_fields));
    dict_segment.SetCompressionKinds(
        {mimicdb::ColumnCompressionKind::kDictionary});
    const auto& dict_cols = dict_segment.CompressedColumns();
    assert(!dict_cols.empty());
    mimicdb::CompressionPredicate dict_pred;
    dict_pred.type = mimicdb::FieldType::kInt64;
    dict_pred.op = mimicdb::CompareOp::kEq;
    dict_pred.i64 = 10;
    mimicdb::Mask dict_mask;
    assert(mimicdb::BuildMaskCompressed(dict_cols[0], dict_pred, &dict_mask));
    assert(dict_mask.Get(0));
    assert(!dict_mask.Get(1));
    assert(dict_mask.Get(2));
    assert(!dict_mask.Get(3));

    mimicdb::FieldVector bit_field("bits", mimicdb::FieldType::kInt64);
    bit_field.AppendInt64(1);
    bit_field.AppendInt64(2);
    bit_field.AppendInt64(3);
    bit_field.AppendInt64(4);
    std::vector<mimicdb::FieldVector> bit_fields;
    bit_fields.push_back(bit_field);
    mimicdb::Segment bit_segment(4, 4, std::move(bit_fields));
    bit_segment.SetCompressionKinds({mimicdb::ColumnCompressionKind::kBitPacked});
    const auto& bit_cols = bit_segment.CompressedColumns();
    assert(!bit_cols.empty());
    mimicdb::CompressionPredicate bit_pred;
    bit_pred.type = mimicdb::FieldType::kInt64;
    bit_pred.op = mimicdb::CompareOp::kGt;
    bit_pred.i64 = 2;
    mimicdb::Mask bit_mask;
    assert(mimicdb::BuildMaskCompressed(bit_cols[0], bit_pred, &bit_mask));
    assert(!bit_mask.Get(0));
    assert(!bit_mask.Get(1));
    assert(bit_mask.Get(2));
    assert(bit_mask.Get(3));

    mimicdb::FieldVector delta_field("delta", mimicdb::FieldType::kInt64);
    delta_field.AppendInt64(1000);
    delta_field.AppendInt64(1001);
    delta_field.AppendInt64(1002);
    delta_field.AppendInt64(1003);
    std::vector<mimicdb::FieldVector> delta_fields;
    delta_fields.push_back(delta_field);
    mimicdb::Segment delta_segment(4, 4, std::move(delta_fields));
    delta_segment.SetCompressionKinds({mimicdb::ColumnCompressionKind::kForDelta});
    const auto& delta_cols = delta_segment.CompressedColumns();
    assert(!delta_cols.empty());
    mimicdb::CompressionPredicate delta_pred;
    delta_pred.type = mimicdb::FieldType::kInt64;
    delta_pred.op = mimicdb::CompareOp::kLe;
    delta_pred.i64 = 1001;
    mimicdb::Mask delta_mask;
    assert(mimicdb::BuildMaskCompressed(delta_cols[0], delta_pred, &delta_mask));
    assert(delta_mask.Get(0));
    assert(delta_mask.Get(1));
    assert(!delta_mask.Get(2));
    assert(!delta_mask.Get(3));

    return 0;
}
