#ifndef MIMICAPI_CORE_H
#define MIMICAPI_CORE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "mimicdb/dataset.h"
#include "mimicdb/predicate.h"

namespace mimicapi {

struct FieldDef {
    std::string name;
    mimicdb::FieldType type;
};

struct Predicate {
    size_t field_index = 0;
    mimicdb::CompareOp op = mimicdb::CompareOp::kEq;
    double value = 0.0;
    mimicdb::FieldType value_type = mimicdb::FieldType::kFloat64;
    std::string bytes;
    bool is_null_check = false;
    bool null_is = true;
};

struct AggregateResult {
    double sum = 0.0;
    double min = 0.0;
    double max = 0.0;
    uint64_t count = 0;
    bool has_value = false;
    size_t rows_scanned = 0;
    size_t rows_pruned = 0;
};

enum class AggregateKind {
    kCount,
    kSum,
    kMin,
    kMax,
};

struct AggregateRequest {
    AggregateKind kind = AggregateKind::kCount;
    size_t field_index = 0;
    std::string alias;
};

struct AggregateMultiResult {
    std::vector<AggregateRequest> requests;
    std::vector<AggregateResult> results;
    uint64_t rows_scanned = 0;
    uint64_t rows_pruned = 0;
};

struct CompressionStats {
    uint64_t raw_bytes = 0;
    uint64_t compressed_bytes = 0;
    size_t segments = 0;
    size_t compressed_segments = 0;
    size_t compressed_columns = 0;
    size_t active_rows = 0;
};

struct ScanResult {
    std::vector<std::string> columns;
    std::vector<std::vector<mimicdb::FieldValue>> rows;
    uint64_t rows_scanned = 0;
    uint64_t rows_pruned = 0;
};

class ApiClientCore {
public:
    bool CreateDatabase(const std::string& name);
    bool CreateDataset(const std::string& db, const std::string& name,
                       const std::vector<FieldDef>& fields);
    bool AddFields(const std::string& db, const std::string& name,
                   const std::vector<FieldDef>& fields, std::string* error);
    bool DropDatabase(const std::string& name);
    bool DropDataset(const std::string& db, const std::string& name);
    const std::vector<FieldDef>* FieldsFor(const std::string& db, const std::string& name) const;
    bool AppendBatch(const std::string& db, const std::string& name,
                     const std::vector<mimicdb::FieldBatch>& batches,
                     std::string* error);
    ScanResult Scan(const std::string& db, const std::string& name,
                    const std::vector<std::string>& columns,
                    const std::vector<Predicate>& predicates, size_t limit,
                    size_t offset, std::string* error) const;
    AggregateResult Aggregate(const std::string& db, const std::string& name,
                              size_t field_index,
                              const std::vector<Predicate>& predicates,
                              std::string* error) const;
    AggregateMultiResult AggregateMulti(const std::string& db, const std::string& name,
                                        const std::vector<AggregateRequest>& requests,
                                        const std::vector<Predicate>& predicates,
                                        std::string* error) const;
    CompressionStats CompressionStatsFor(const std::string& db, const std::string& name,
                                         std::string* error) const;

private:
    struct DatasetState {
        mimicdb::Dataset dataset;
        std::vector<FieldDef> fields;
        std::unordered_map<std::string, size_t> field_index;
        explicit DatasetState(std::string name) : dataset(std::move(name)) {}
    };

    std::unordered_map<std::string, std::unordered_map<std::string, DatasetState>> databases_;

    const DatasetState* GetDataset(const std::string& db, const std::string& name) const;
    DatasetState* GetDataset(const std::string& db, const std::string& name);
};

}  // namespace mimicapi

#endif  // MIMICAPI_CORE_H
