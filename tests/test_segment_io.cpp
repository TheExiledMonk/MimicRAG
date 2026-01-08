#include <cassert>
#include <cstdio>
#include <string>

#include "mimicdb/dataset.h"
#include "mimicdb/segment.h"
#include "mimicdb/scan.h"
#include "mimicdb/segment_io.h"
#include "mimicdb/types.h"

int main() {
    mimicdb::Dataset users("users");
    users.AddField(mimicdb::FieldVector("age", mimicdb::FieldType::kInt32));
    users.AddField(mimicdb::FieldVector("income", mimicdb::FieldType::kFloat64));
    users.AddField(mimicdb::FieldVector("name", mimicdb::FieldType::kString));

    assert(users.Append({mimicdb::FieldValue::Int32(34),
                         mimicdb::FieldValue::Float64(100.5),
                         mimicdb::FieldValue::String("alice")}));
    assert(users.Append({mimicdb::FieldValue::Int32(21),
                         mimicdb::FieldValue::Null(mimicdb::FieldType::kFloat64),
                         mimicdb::FieldValue::String("bob")}));

    mimicdb::Segment segment(8, users.RowCount(), users.Fields());

    const std::string path = "tests/segment_roundtrip.bin";
    mimicdb::SegmentWriter writer(path);
    assert(writer.IsOpen());
    assert(writer.Write(segment));

    mimicdb::Segment loaded(0, {});
    mimicdb::SegmentReader reader(path);
    reader.SetExpectedSchemaFingerprint(segment.SchemaFingerprint());
    assert(reader.Read(&loaded));
    assert(loaded.RowCount() == 2);
    assert(loaded.Fields().size() == 3);

    const auto& columns = reader.ColumnHeaders();
    assert(columns.size() == 3);
    assert(columns[0].null_count == 0);
    assert(columns[0].has_value == 1);
    assert(columns[0].min == 21.0);
    assert(columns[0].max == 34.0);
    assert(columns[1].null_count == 1);
    assert(columns[1].has_value == 1);
    assert(columns[1].min == 100.5);
    assert(columns[1].max == 100.5);
    assert(columns[2].has_value == 0);

    mimicdb::SegmentColumnStats age_stats;
    age_stats.min = columns[0].min;
    age_stats.max = columns[0].max;
    age_stats.null_count = columns[0].null_count;
    age_stats.has_value = columns[0].has_value != 0;
    assert(mimicdb::SegmentMatchesPredicate(age_stats, mimicdb::CompareOp::kEq, 34.0));
    assert(!mimicdb::SegmentMatchesPredicate(age_stats, mimicdb::CompareOp::kEq, 99.0));

    mimicdb::FieldVector age2("age", mimicdb::FieldType::kInt32);
    age2.AppendInt32(100);
    age2.AppendInt32(101);
    mimicdb::Segment segment2(8, 2, std::vector<mimicdb::FieldVector>{age2});
    std::vector<mimicdb::Segment> segments = {segment, segment2};
    auto to_scan = mimicdb::PruneSegmentsByPredicate(segments, 0, mimicdb::CompareOp::kEq, 34.0);
    assert(to_scan.size() == 1);
    assert(to_scan[0] == 0);

    const auto& age = loaded.Fields()[0];
    const auto& income = loaded.Fields()[1];
    const auto& name = loaded.Fields()[2];
    assert(age.Type() == mimicdb::FieldType::kInt32);
    assert(income.Type() == mimicdb::FieldType::kFloat64);
    assert(name.Type() == mimicdb::FieldType::kString);
    assert(age.DataInt32()[0] == 34);
    assert(age.DataInt32()[1] == 21);
    assert(income.DataFloat64()[0] == 100.5);
    assert(!income.IsValid(1));
    const auto* lengths = name.DataLengths();
    const auto* bytes = name.DataBytes();
    assert(lengths[0] == 5);
    assert(lengths[1] == 3);
    assert(std::string(reinterpret_cast<const char*>(bytes), lengths[0]) == "alice");

    std::remove(path.c_str());
    return 0;
}
