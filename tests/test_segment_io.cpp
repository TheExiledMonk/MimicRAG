#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>

#include "mimicdb/dataset.h"
#include "mimicdb/segment.h"
#include "mimicdb/scan.h"
#include "mimicdb/segment_io.h"
#include "mimicdb/types.h"

#define REQUIRE(condition) \
    do { \
        if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

int main() {
    mimicdb::Dataset users("users");
    users.AddField(mimicdb::FieldVector("age", mimicdb::FieldType::kInt32));
    users.AddField(mimicdb::FieldVector("income", mimicdb::FieldType::kFloat64));
    users.AddField(mimicdb::FieldVector("name", mimicdb::FieldType::kString));
    users.AddField(mimicdb::FieldVector("active", mimicdb::FieldType::kBool));

    REQUIRE(users.Append({mimicdb::FieldValue::Int32(34),
                         mimicdb::FieldValue::Float64(100.5),
                         mimicdb::FieldValue::String("alice"),
                         mimicdb::FieldValue::Bool(true)}));
    REQUIRE(users.Append({mimicdb::FieldValue::Int32(21),
                         mimicdb::FieldValue::Null(mimicdb::FieldType::kFloat64),
                         mimicdb::FieldValue::String("bo"),
                         mimicdb::FieldValue::Null(mimicdb::FieldType::kBool)}));

    mimicdb::Segment segment(8, users.RowCount(), users.Fields());

    std::random_device random;
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
        std::to_string(random());
    const std::string path =
        (std::filesystem::temp_directory_path() / ("mimicdb_segment_roundtrip_" + unique + ".bin")).string();
    mimicdb::SegmentWriter writer(path);
    REQUIRE(writer.IsOpen());
    REQUIRE(writer.Write(segment));

    mimicdb::Segment loaded(0, {});
    mimicdb::SegmentReader reader(path);
    reader.SetExpectedSchemaFingerprint(segment.SchemaFingerprint());
    REQUIRE(reader.Read(&loaded));
    REQUIRE(loaded.RowCount() == 2);
    REQUIRE(loaded.Fields().size() == 4);

    const auto& columns = reader.ColumnHeaders();
    REQUIRE(columns.size() == 4);
    REQUIRE(columns[0].null_count == 0);
    REQUIRE(columns[0].has_value == 1);
    REQUIRE(columns[0].min == 21.0);
    REQUIRE(columns[0].max == 34.0);
    REQUIRE(columns[1].null_count == 1);
    REQUIRE(columns[1].has_value == 1);
    REQUIRE(columns[1].min == 100.5);
    REQUIRE(columns[1].max == 100.5);
    REQUIRE(columns[2].has_value == 0);
    REQUIRE(columns[3].null_count == 1);

    mimicdb::SegmentColumnStats age_stats;
    age_stats.min = columns[0].min;
    age_stats.max = columns[0].max;
    age_stats.null_count = columns[0].null_count;
    age_stats.has_value = columns[0].has_value != 0;
    REQUIRE(mimicdb::SegmentMatchesPredicate(age_stats, mimicdb::CompareOp::kEq, 34.0));
    REQUIRE(!mimicdb::SegmentMatchesPredicate(age_stats, mimicdb::CompareOp::kEq, 99.0));

    mimicdb::FieldVector age2("age", mimicdb::FieldType::kInt32);
    age2.AppendInt32(100);
    age2.AppendInt32(101);
    mimicdb::Segment segment2(8, 2, std::vector<mimicdb::FieldVector>{age2});
    std::vector<mimicdb::Segment> segments = {segment, segment2};
    auto to_scan = mimicdb::PruneSegmentsByPredicate(segments, 0, mimicdb::CompareOp::kEq, 34.0);
    REQUIRE(to_scan.size() == 1);
    REQUIRE(to_scan[0] == 0);

    const auto& age = loaded.Fields()[0];
    const auto& income = loaded.Fields()[1];
    const auto& name = loaded.Fields()[2];
    const auto& active = loaded.Fields()[3];
    REQUIRE(age.Type() == mimicdb::FieldType::kInt32);
    REQUIRE(income.Type() == mimicdb::FieldType::kFloat64);
    REQUIRE(name.Type() == mimicdb::FieldType::kString);
    REQUIRE(active.Type() == mimicdb::FieldType::kBool);
    REQUIRE(age.DataInt32()[0] == 34);
    REQUIRE(age.DataInt32()[1] == 21);
    REQUIRE(income.DataFloat64()[0] == 100.5);
    REQUIRE(!income.IsValid(1));
    const auto* lengths = name.DataLengths();
    const auto* bytes = name.DataBytes();
    REQUIRE(lengths[0] == 5);
    REQUIRE(lengths[1] == 2);
    REQUIRE(std::string(reinterpret_cast<const char*>(bytes), lengths[0]) == "alice");
    REQUIRE(active.DataBool()[0] == 1);
    REQUIRE(!active.IsValid(1));

    std::remove(path.c_str());
    return 0;
}
