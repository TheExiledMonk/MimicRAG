#include <cassert>
#include <cstdio>
#include <string>

#include "pcdb/dataset.h"
#include "pcdb/segment.h"
#include "pcdb/scan.h"
#include "pcdb/segment_io.h"
#include "pcdb/types.h"

int main() {
    pcdb::Dataset users("users");
    users.AddField(pcdb::FieldVector("age", pcdb::FieldType::kInt32));
    users.AddField(pcdb::FieldVector("income", pcdb::FieldType::kFloat64));

    assert(users.Append({pcdb::FieldValue::Int32(34), pcdb::FieldValue::Float64(100.5)}));
    assert(users.Append({pcdb::FieldValue::Int32(21), pcdb::FieldValue::Null(pcdb::FieldType::kFloat64)}));

    pcdb::Segment segment(8, users.RowCount(), users.Fields());

    const std::string path = "tests/segment_roundtrip.bin";
    pcdb::SegmentWriter writer(path);
    assert(writer.IsOpen());
    assert(writer.Write(segment));

    pcdb::Segment loaded(0, {});
    pcdb::SegmentReader reader(path);
    reader.SetExpectedSchemaFingerprint(segment.SchemaFingerprint());
    assert(reader.Read(&loaded));
    assert(loaded.RowCount() == 2);
    assert(loaded.Fields().size() == 2);

    const auto& columns = reader.ColumnHeaders();
    assert(columns.size() == 2);
    assert(columns[0].null_count == 0);
    assert(columns[0].has_value == 1);
    assert(columns[0].min == 21.0);
    assert(columns[0].max == 34.0);
    assert(columns[1].null_count == 1);
    assert(columns[1].has_value == 1);
    assert(columns[1].min == 100.5);
    assert(columns[1].max == 100.5);

    pcdb::SegmentColumnStats age_stats;
    age_stats.min = columns[0].min;
    age_stats.max = columns[0].max;
    age_stats.null_count = columns[0].null_count;
    age_stats.has_value = columns[0].has_value != 0;
    assert(pcdb::SegmentMatchesPredicate(age_stats, pcdb::CompareOp::kEq, 34.0));
    assert(!pcdb::SegmentMatchesPredicate(age_stats, pcdb::CompareOp::kEq, 99.0));

    pcdb::FieldVector age2("age", pcdb::FieldType::kInt32);
    age2.AppendInt32(100);
    age2.AppendInt32(101);
    pcdb::Segment segment2(8, 2, std::vector<pcdb::FieldVector>{age2});
    std::vector<pcdb::Segment> segments = {segment, segment2};
    auto to_scan = pcdb::PruneSegmentsByPredicate(segments, 0, pcdb::CompareOp::kEq, 34.0);
    assert(to_scan.size() == 1);
    assert(to_scan[0] == 0);

    const auto& age = loaded.Fields()[0];
    const auto& income = loaded.Fields()[1];
    assert(age.Type() == pcdb::FieldType::kInt32);
    assert(income.Type() == pcdb::FieldType::kFloat64);
    assert(age.DataInt32()[0] == 34);
    assert(age.DataInt32()[1] == 21);
    assert(income.DataFloat64()[0] == 100.5);
    assert(!income.IsValid(1));

    std::remove(path.c_str());
    return 0;
}
