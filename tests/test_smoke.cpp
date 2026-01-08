#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

#include "mimicdb/bitmap.h"
#include "mimicdb/dataset.h"
#include "mimicdb/segment.h"
#include "mimicdb/segment_io.h"
#include "mimicdb/types.h"
#include "mimicdb/field_vector.h"

int main() {
    mimicdb::Bitmap bitmap(10);
    assert(bitmap.Size() == 10);
    assert(!bitmap.Get(3));
    bitmap.Set(3, true);
    assert(bitmap.Get(3));
    bitmap.Set(3, false);
    assert(!bitmap.Get(3));

    mimicdb::FieldVector ages("age", mimicdb::FieldType::kInt32);
    ages.Resize(4);
    assert(ages.Size() == 4);
    assert(!ages.HasNulls());
    assert(ages.IsValid(2));

    ages.SetValid(2, false);
    assert(ages.HasNulls());
    assert(!ages.IsValid(2));
    assert(ages.IsValid(1));

    mimicdb::Dataset users("users");
    users.AddField(mimicdb::FieldVector("age", mimicdb::FieldType::kInt32));
    users.AddField(mimicdb::FieldVector("country", mimicdb::FieldType::kInt32));
    assert(users.Name() == "users");
    assert(users.Fields().size() == 2);
    assert(users.RowCount() == 0);
    assert(users.Append({mimicdb::FieldValue::Int32(34), mimicdb::FieldValue::Int32(45)}));
    assert(users.Append({mimicdb::FieldValue::Int32(21), mimicdb::FieldValue::Int32(45)}));
    assert(users.RowCount() == 2);
    assert(users.Fields()[0].Size() == 2);
    assert(users.Fields()[1].Size() == 2);

    mimicdb::Segment segment(8, users.Fields());
    assert(segment.RowCapacity() == 8);
    assert(segment.RowCount() == 0);
    assert(segment.Format().row_capacity == 8);
    assert(segment.Append(4));
    assert(segment.RowCount() == 4);
    assert(!segment.Append(5));
    assert(segment.Append(4));
    assert(segment.RowCount() == 8);

    const std::string path = "tests/segment_smoke.bin";
    mimicdb::SegmentWriter writer(path);
    assert(writer.IsOpen());
    assert(writer.Write(segment));

    mimicdb::Segment read_back(0, {});
    mimicdb::SegmentReader reader(path);
    assert(reader.IsOpen());
    assert(reader.Read(&read_back));
    assert(read_back.RowCapacity() == 8);
    assert(read_back.RowCount() == 8);
    std::remove(path.c_str());

    mimicdb::SegmentReader fingerprint_ok(path);
    fingerprint_ok.SetExpectedSchemaFingerprint(segment.SchemaFingerprint());
    assert(fingerprint_ok.Read(nullptr));

    mimicdb::SegmentReader fingerprint_bad(path);
    fingerprint_bad.SetExpectedSchemaFingerprint(segment.SchemaFingerprint() + 1);
    assert(!fingerprint_bad.Read(nullptr));

    mimicdb::SegmentReader fingerprint_schema_ok(path);
    assert(fingerprint_schema_ok.ReadWithSchema(nullptr, users.SchemaView()));

    mimicdb::Dataset other("other");
    other.AddField(mimicdb::FieldVector("age", mimicdb::FieldType::kInt32));
    mimicdb::SegmentReader fingerprint_schema_bad(path);
    assert(!fingerprint_schema_bad.ReadWithSchema(nullptr, other.SchemaView()));

    const std::string bad_path = "tests/segment_bad_header.bin";
    {
        mimicdb::SegmentHeader bad_header;
        bad_header.magic = 0;
        std::ofstream out(bad_path, std::ios::binary | std::ios::trunc);
        assert(out.is_open());
        out.write(reinterpret_cast<const char*>(&bad_header), sizeof(bad_header));
        assert(out.good());
    }
    mimicdb::SegmentReader bad_reader(bad_path);
    assert(bad_reader.IsOpen());
    assert(!bad_reader.Read(nullptr));
    std::remove(bad_path.c_str());

    const std::string bad_version_path = "tests/segment_bad_version.bin";
    {
        mimicdb::SegmentHeader bad_header;
        bad_header.version = 999;
        std::ofstream out(bad_version_path, std::ios::binary | std::ios::trunc);
        assert(out.is_open());
        out.write(reinterpret_cast<const char*>(&bad_header), sizeof(bad_header));
        assert(out.good());
    }
    mimicdb::SegmentReader bad_version_reader(bad_version_path);
    assert(bad_version_reader.IsOpen());
    assert(!bad_version_reader.Read(nullptr));
    std::remove(bad_version_path.c_str());

    return 0;
}
