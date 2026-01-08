#ifndef MIMICAPI_MONGO_H
#define MIMICAPI_MONGO_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "mimicapi/core.h"

namespace mimicapi {

enum class FilterOp {
    kEq,
    kNe,
    kGt,
    kLt,
    kIn,
    kNin,
    kExists,
    kAll,
    kSize,
    kRegex,
};

enum class MatchOp {
    kAnd,
    kOr,
    kNor,
};

struct MongoDocument {
    std::unordered_map<std::string, mimicdb::FieldValue> fields;
};

struct Filter {
    std::string field;
    FilterOp op = FilterOp::kEq;
    std::vector<mimicdb::FieldValue> values;
    bool exists = true;
    bool negated = false;
    std::string regex;
    std::string regex_options;
};

struct MatchExpression {
    MatchOp op = MatchOp::kAnd;
    std::vector<Filter> filters;
    std::vector<MatchExpression> children;
};

struct AggregateOp {
    std::string name;
    std::string op;
    std::string field;
    bool count_only = false;
};

struct GroupField {
    std::string name;
    std::string field;
};

struct GroupBySpec {
    bool has_id = false;
    std::string field;
    std::vector<GroupField> fields;
};

struct ComputedField {
    std::string name;
    bool from_field = false;
    std::string field;
    mimicdb::FieldValue literal;
};

struct SliceSpec {
    std::string field;
    int32_t skip = 0;
    int32_t limit = 0;
    bool has_limit = false;
};

struct ProjectionSpec {
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    std::vector<ComputedField> computed;
    std::vector<SliceSpec> slices;
};

struct SortSpec {
    std::string field;
    int direction = 1;
};

struct FindOptions {
    std::vector<SortSpec> sort;
    size_t skip = 0;
    size_t limit = 0;
};

enum class StageType {
    kMatch,
    kGroup,
    kCount,
    kSortByCount,
    kAddFields,
    kProject,
    kUnwind,
    kLookup,
    kFacet,
};

enum class UpdateOpType {
    kSet,
    kSetOnInsert,
    kRename,
    kCurrentDate,
    kUnset,
    kPush,
    kPull,
    kAddToSet,
};

struct UpdateOp {
    UpdateOpType type = UpdateOpType::kSet;
    std::string field;
    std::vector<mimicdb::FieldValue> values;
    std::string rename_to;
    bool current_timestamp = false;
    FilterOp pull_op = FilterOp::kEq;
    std::string regex;
    std::string regex_options;
};

struct UpdateSpec {
    bool is_replacement = false;
    std::unordered_map<std::string, mimicdb::FieldValue> replacement;
    std::vector<UpdateOp> ops;
};

struct SortByCountSpec {
    bool is_field = false;
    std::string field;
    mimicdb::FieldValue literal;
};

struct UnwindSpec {
    std::string field;
    bool preserve_null = false;
};

struct LookupSpec {
    std::string from;
    std::string local_field;
    std::string foreign_field;
    std::string as_field;
};

struct PipelineStage;
struct FacetSpec {
    std::vector<std::pair<std::string, std::vector<PipelineStage>>> branches;
};

struct PipelineStage {
    StageType type = StageType::kMatch;
    MatchExpression match;
    GroupBySpec group;
    std::vector<AggregateOp> ops;
    std::string count_field;
    SortByCountSpec sort_by_count;
    std::vector<ComputedField> add_fields;
    ProjectionSpec project;
    UnwindSpec unwind;
    LookupSpec lookup;
    FacetSpec facet;
};


class MongoClientCore {
public:
    explicit MongoClientCore(ApiClientCore* core);

    bool InsertMany(const std::string& db, const std::string& collection,
                    const std::vector<MongoDocument>& docs, std::string* error);
    std::vector<MongoDocument> Find(const std::string& db, const std::string& collection,
                                    const std::vector<Filter>& filters,
                                    const ProjectionSpec& projection,
                                    const FindOptions& options,
                                    std::string* error) const;
    std::vector<MongoDocument> AggregatePipeline(const std::string& db,
                                                 const std::string& collection,
                                                 const std::vector<PipelineStage>& pipeline,
                                                 std::string* error) const;
    size_t Update(const std::string& db, const std::string& collection,
                  const std::vector<Filter>& filters,
                  const UpdateSpec& update,
                  bool multi, bool upsert, bool replace, std::string* error);
    size_t Delete(const std::string& db, const std::string& collection,
                  const std::vector<Filter>& filters, bool multi, std::string* error);

private:
    struct CollectionState {
        std::vector<FieldDef> fields;
        std::unordered_map<std::string, size_t> field_index;
        int64_t next_id = 1;
        bool initialized = false;
    };

    ApiClientCore* core_;
    std::unordered_map<std::string, std::unordered_map<std::string, CollectionState>> collections_;
    uint64_t version_counter_ = 0;

    CollectionState* GetCollection(const std::string& db, const std::string& collection);
    const CollectionState* GetCollection(const std::string& db, const std::string& collection) const;
    bool EnsureSchema(const std::string& db, const std::string& collection,
                      const std::vector<MongoDocument>& docs, std::string* error);
    uint64_t NextVersion();
    std::vector<MongoDocument> LatestDocuments(const std::string& db,
                                               const std::string& collection,
                                               std::string* error) const;
};

}  // namespace mimicapi

#endif  // MIMICAPI_MONGO_H
