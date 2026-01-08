#include <cassert>

#include "mimicdb/aggregate.h"
#include "mimicdb/field_vector.h"
#include "mimicdb/types.h"

int main() {
    mimicdb::FieldVector ages("age", mimicdb::FieldType::kInt32);
    assert(ages.AppendInt32(10));
    assert(ages.AppendInt32(20));
    assert(ages.Size() == 2);
    assert(!ages.HasNulls());

    assert(ages.AppendNull());
    assert(ages.Size() == 3);
    assert(ages.HasNulls());
    assert(ages.IsValid(0));
    assert(!ages.IsValid(2));

    mimicdb::FieldVector flags("flag", mimicdb::FieldType::kBool);
    assert(flags.AppendBool(true));
    assert(flags.AppendBool(false));
    assert(flags.Size() == 2);

    const int32_t batch_values[] = {1, 2, 3, 4};
    const uint8_t batch_valid[] = {1, 0, 1, 1};
    mimicdb::FieldVector batch("batch", mimicdb::FieldType::kInt32);
    assert(batch.AppendBatchInt32(batch_values, 4, batch_valid));
    assert(batch.Size() == 4);
    assert(batch.HasNulls());
    assert(batch.IsValid(0));
    assert(!batch.IsValid(1));
    assert(batch.IsValid(2));

    mimicdb::FieldVector dict("country", mimicdb::FieldType::kDictInt32);
    assert(dict.AppendDictInt32(7));
    assert(dict.AppendDictInt32(9));
    assert(dict.AppendDictInt32(7));
    assert(dict.Size() == 3);
    assert(dict.DictionarySize() == 2);
    assert(dict.DataDictIds()[0] == dict.DataDictIds()[2]);
    assert(dict.DictionaryValue(dict.DataDictIds()[1]) == 9);

    mimicdb::FieldVector names("name", mimicdb::FieldType::kString);
    assert(names.AppendString("alice"));
    assert(names.AppendNull());
    assert(names.AppendString("bob"));
    assert(names.Size() == 3);
    assert(names.HasNulls());
    const uint32_t* name_lengths = names.DataLengths();
    assert(name_lengths[0] == 5);
    assert(name_lengths[1] == 0);
    assert(name_lengths[2] == 3);

    mimicdb::AggregateResult non_null;
    mimicdb::AggregateField(ages, nullptr, &non_null);
    assert(non_null.count == 2);

    mimicdb::AggregateResult nullable;
    mimicdb::AggregateField(batch, nullptr, &nullable);
    assert(nullable.count == 3);

    return 0;
}
