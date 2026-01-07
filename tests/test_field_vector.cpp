#include <cassert>

#include "pcdb/aggregate.h"
#include "pcdb/field_vector.h"
#include "pcdb/types.h"

int main() {
    pcdb::FieldVector ages("age", pcdb::FieldType::kInt32);
    assert(ages.AppendInt32(10));
    assert(ages.AppendInt32(20));
    assert(ages.Size() == 2);
    assert(!ages.HasNulls());

    assert(ages.AppendNull());
    assert(ages.Size() == 3);
    assert(ages.HasNulls());
    assert(ages.IsValid(0));
    assert(!ages.IsValid(2));

    pcdb::FieldVector flags("flag", pcdb::FieldType::kBool);
    assert(flags.AppendBool(true));
    assert(flags.AppendBool(false));
    assert(flags.Size() == 2);

    const int32_t batch_values[] = {1, 2, 3, 4};
    const uint8_t batch_valid[] = {1, 0, 1, 1};
    pcdb::FieldVector batch("batch", pcdb::FieldType::kInt32);
    assert(batch.AppendBatchInt32(batch_values, 4, batch_valid));
    assert(batch.Size() == 4);
    assert(batch.HasNulls());
    assert(batch.IsValid(0));
    assert(!batch.IsValid(1));
    assert(batch.IsValid(2));

    pcdb::FieldVector dict("country", pcdb::FieldType::kDictInt32);
    assert(dict.AppendDictInt32(7));
    assert(dict.AppendDictInt32(9));
    assert(dict.AppendDictInt32(7));
    assert(dict.Size() == 3);
    assert(dict.DictionarySize() == 2);
    assert(dict.DataDictIds()[0] == dict.DataDictIds()[2]);
    assert(dict.DictionaryValue(dict.DataDictIds()[1]) == 9);

    pcdb::AggregateResult non_null;
    pcdb::AggregateField(ages, nullptr, &non_null);
    assert(non_null.count == 2);

    pcdb::AggregateResult nullable;
    pcdb::AggregateField(batch, nullptr, &nullable);
    assert(nullable.count == 3);

    return 0;
}
