#include "mimicapi/mongo.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <functional>
#include <regex>
#include <unordered_set>

#include "mimicdb/array_codec.h"
namespace mimicapi {

namespace {

bool IsScalar(mimicdb::FieldType type) {
    return type == mimicdb::FieldType::kInt32 ||
           type == mimicdb::FieldType::kInt64 ||
           type == mimicdb::FieldType::kFloat64 ||
           type == mimicdb::FieldType::kBool ||
           type == mimicdb::FieldType::kString ||
           type == mimicdb::FieldType::kBytes ||
           type == mimicdb::FieldType::kArray;
}

FieldDef InferField(const std::string& name, const mimicdb::FieldValue& value) {
    FieldDef def;
    def.name = name;
    def.type = value.type;
    return def;
}

bool IsNullValue(const mimicdb::FieldValue& value) {
    return value.is_null;
}

bool ValuesEqual(const mimicdb::FieldValue& left, const mimicdb::FieldValue& right) {
    if (left.is_null || right.is_null) {
        return false;
    }
    if (left.type != right.type) {
        return false;
    }
    switch (left.type) {
        case mimicdb::FieldType::kInt32:
            return left.i32 == right.i32;
        case mimicdb::FieldType::kInt64:
            return left.i64 == right.i64;
        case mimicdb::FieldType::kFloat64:
            return left.f64 == right.f64;
        case mimicdb::FieldType::kBool:
            return left.b == right.b;
        case mimicdb::FieldType::kString:
        case mimicdb::FieldType::kBytes:
            return left.bytes == right.bytes;
        case mimicdb::FieldType::kArray:
            if (left.array.size() != right.array.size()) {
                return false;
            }
            for (size_t i = 0; i < left.array.size(); ++i) {
                if (!ValuesEqual(left.array[i], right.array[i])) {
                    return false;
                }
            }
            return true;
        case mimicdb::FieldType::kObject:
            if (left.object.size() != right.object.size()) {
                return false;
            }
            for (const auto& item : left.object) {
                auto it = right.object.find(item.first);
                if (it == right.object.end()) {
                    return false;
                }
                if (!ValuesEqual(item.second, it->second)) {
                    return false;
                }
            }
            return true;
        case mimicdb::FieldType::kDictInt32:
            return left.i32 == right.i32;
    }
    return false;
}

std::vector<std::string> SplitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : path) {
        if (ch == '.') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

bool ParseIndex(const std::string& part, size_t* index) {
    if (!index || part.empty()) {
        return false;
    }
    size_t value = 0;
    for (char ch : part) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        value = value * 10 + static_cast<size_t>(ch - '0');
    }
    *index = value;
    return true;
}

bool GetPathValue(const mimicdb::FieldValue& value,
                  const std::vector<std::string>& parts,
                  size_t index,
                  mimicdb::FieldValue* out) {
    if (index >= parts.size()) {
        if (out) {
            *out = value;
        }
        return true;
    }
    if (value.is_null) {
        return false;
    }
    const std::string& part = parts[index];
    if (value.type == mimicdb::FieldType::kObject) {
        auto it = value.object.find(part);
        if (it == value.object.end()) {
            return false;
        }
        return GetPathValue(it->second, parts, index + 1, out);
    }
    if (value.type == mimicdb::FieldType::kArray) {
        size_t idx = 0;
        if (!ParseIndex(part, &idx) || idx >= value.array.size()) {
            return false;
        }
        return GetPathValue(value.array[idx], parts, index + 1, out);
    }
    return false;
}

bool GetPathValue(const MongoDocument& doc,
                  const std::vector<std::string>& parts,
                  mimicdb::FieldValue* out) {
    if (parts.empty()) {
        return false;
    }
    auto it = doc.fields.find(parts[0]);
    if (it == doc.fields.end()) {
        return false;
    }
    if (parts.size() == 1) {
        if (out) {
            *out = it->second;
        }
        return true;
    }
    return GetPathValue(it->second, parts, 1, out);
}

bool SetPathValue(mimicdb::FieldValue* value,
                  const std::vector<std::string>& parts,
                  size_t index,
                  const mimicdb::FieldValue& new_value,
                  std::string* error);

bool SetPathArray(std::vector<mimicdb::FieldValue>* array,
                  const std::vector<std::string>& parts,
                  size_t index,
                  const mimicdb::FieldValue& new_value,
                  std::string* error) {
    if (!array) {
        return false;
    }
    if (index >= parts.size()) {
        return false;
    }
    size_t idx = 0;
    if (!ParseIndex(parts[index], &idx)) {
        if (error) {
            *error = "array update requires numeric index";
        }
        return false;
    }
    if (array->size() <= idx) {
        array->resize(idx + 1, mimicdb::FieldValue::Object({}));
    }
    if (index + 1 == parts.size()) {
        (*array)[idx] = new_value;
        return true;
    }
    auto* child = &(*array)[idx];
    if (child->is_null || (child->type != mimicdb::FieldType::kObject &&
                           child->type != mimicdb::FieldType::kArray)) {
        *child = mimicdb::FieldValue::Object({});
    }
    return SetPathValue(child, parts, index + 1, new_value, error);
}

bool SetPathObject(std::unordered_map<std::string, mimicdb::FieldValue>* object,
                   const std::vector<std::string>& parts,
                   size_t index,
                   const mimicdb::FieldValue& new_value,
                   std::string* error) {
    if (!object) {
        return false;
    }
    if (index >= parts.size()) {
        return false;
    }
    const std::string& key = parts[index];
    if (index + 1 == parts.size()) {
        (*object)[key] = new_value;
        return true;
    }
    auto& child = (*object)[key];
    if (child.is_null || (child.type != mimicdb::FieldType::kObject &&
                          child.type != mimicdb::FieldType::kArray)) {
        child = mimicdb::FieldValue::Object({});
    }
    return SetPathValue(&child, parts, index + 1, new_value, error);
}

bool SetPathValue(mimicdb::FieldValue* value,
                  const std::vector<std::string>& parts,
                  size_t index,
                  const mimicdb::FieldValue& new_value,
                  std::string* error) {
    if (!value) {
        return false;
    }
    if (index >= parts.size()) {
        *value = new_value;
        return true;
    }
    if (value->is_null || (value->type != mimicdb::FieldType::kObject &&
                           value->type != mimicdb::FieldType::kArray)) {
        *value = mimicdb::FieldValue::Object({});
    }
    if (value->type == mimicdb::FieldType::kObject) {
        return SetPathObject(&value->object, parts, index, new_value, error);
    }
    if (value->type == mimicdb::FieldType::kArray) {
        return SetPathArray(&value->array, parts, index, new_value, error);
    }
    if (error) {
        *error = "cannot set nested path on non-document";
    }
    return false;
}

bool SetPathValue(MongoDocument* doc,
                  const std::vector<std::string>& parts,
                  const mimicdb::FieldValue& new_value,
                  std::string* error) {
    if (!doc || parts.empty()) {
        return false;
    }
    if (parts.size() == 1) {
        doc->fields[parts[0]] = new_value;
        return true;
    }
    auto& root = doc->fields[parts[0]];
    return SetPathValue(&root, parts, 1, new_value, error);
}

bool DeletePathValue(mimicdb::FieldValue* value,
                     const std::vector<std::string>& parts,
                     size_t index);

bool DeletePathArray(std::vector<mimicdb::FieldValue>* array,
                     const std::vector<std::string>& parts,
                     size_t index) {
    if (!array || index >= parts.size()) {
        return false;
    }
    size_t idx = 0;
    if (!ParseIndex(parts[index], &idx) || idx >= array->size()) {
        return false;
    }
    if (index + 1 == parts.size()) {
        array->erase(array->begin() + static_cast<std::ptrdiff_t>(idx));
        return true;
    }
    return DeletePathValue(&(*array)[idx], parts, index + 1);
}

bool DeletePathObject(std::unordered_map<std::string, mimicdb::FieldValue>* object,
                      const std::vector<std::string>& parts,
                      size_t index) {
    if (!object || index >= parts.size()) {
        return false;
    }
    const std::string& key = parts[index];
    auto it = object->find(key);
    if (it == object->end()) {
        return false;
    }
    if (index + 1 == parts.size()) {
        object->erase(it);
        return true;
    }
    return DeletePathValue(&it->second, parts, index + 1);
}

bool DeletePathValue(mimicdb::FieldValue* value,
                     const std::vector<std::string>& parts,
                     size_t index) {
    if (!value || value->is_null) {
        return false;
    }
    if (value->type == mimicdb::FieldType::kObject) {
        return DeletePathObject(&value->object, parts, index);
    }
    if (value->type == mimicdb::FieldType::kArray) {
        return DeletePathArray(&value->array, parts, index);
    }
    return false;
}

bool DeletePathValue(MongoDocument* doc, const std::vector<std::string>& parts) {
    if (!doc || parts.empty()) {
        return false;
    }
    if (parts.size() == 1) {
        return doc->fields.erase(parts[0]) > 0;
    }
    auto it = doc->fields.find(parts[0]);
    if (it == doc->fields.end()) {
        return false;
    }
    return DeletePathValue(&it->second, parts, 1);
}

bool RenamePathValue(MongoDocument* doc, const std::vector<std::string>& from,
                     const std::vector<std::string>& to,
                     std::string* error) {
    mimicdb::FieldValue value;
    if (!GetPathValue(*doc, from, &value)) {
        return true;
    }
    DeletePathValue(doc, from);
    return SetPathValue(doc, to, value, error);
}

bool MatchPullValue(const mimicdb::FieldValue& item, const UpdateOp& op) {
    switch (op.pull_op) {
        case FilterOp::kIn:
            for (const auto& candidate : op.values) {
                if (ValuesEqual(item, candidate)) {
                    return true;
                }
            }
            return false;
        case FilterOp::kNe:
            return op.values.empty() || !ValuesEqual(item, op.values[0]);
        case FilterOp::kRegex: {
            if (item.is_null || item.type != mimicdb::FieldType::kString) {
                return false;
            }
            std::regex::flag_type flags = std::regex::ECMAScript;
            if (op.regex_options.find('i') != std::string::npos) {
                flags |= std::regex::icase;
            }
            if (op.regex_options.find('m') != std::string::npos) {
                flags |= std::regex::multiline;
            }
            if (op.regex_options.find('s') != std::string::npos) {
                flags |= std::regex::ECMAScript;
            }
            std::regex re(op.regex, flags);
            return std::regex_search(item.bytes, re);
        }
        case FilterOp::kEq:
        default:
            return !op.values.empty() && ValuesEqual(item, op.values[0]);
    }
}

bool ApplyUpdateOps(MongoDocument* doc, const UpdateSpec& update, bool upsert,
                    std::string* error) {
    for (const auto& op : update.ops) {
        const std::vector<std::string> parts = SplitPath(op.field);
        if (parts.empty()) {
            if (error) {
                *error = "empty update path";
            }
            return false;
        }
        switch (op.type) {
            case UpdateOpType::kSet:
                if (op.values.empty()) {
                    continue;
                }
                if (!SetPathValue(doc, parts, op.values[0], error)) {
                    return false;
                }
                break;
            case UpdateOpType::kSetOnInsert:
                if (!upsert || op.values.empty()) {
                    continue;
                }
                if (!SetPathValue(doc, parts, op.values[0], error)) {
                    return false;
                }
                break;
            case UpdateOpType::kRename:
                if (!RenamePathValue(doc, parts, SplitPath(op.rename_to), error)) {
                    return false;
                }
                break;
            case UpdateOpType::kCurrentDate: {
                const auto now = std::chrono::system_clock::now();
                if (op.current_timestamp) {
                    const auto nanos =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
                    if (!SetPathValue(doc, parts,
                                      mimicdb::FieldValue::Int64(static_cast<int64_t>(nanos)),
                                      error)) {
                        return false;
                    }
                } else {
                    const auto seconds =
                        std::chrono::duration_cast<std::chrono::duration<double>>(
                            now.time_since_epoch()).count();
                    if (!SetPathValue(doc, parts,
                                      mimicdb::FieldValue::Float64(seconds),
                                      error)) {
                        return false;
                    }
                }
                break;
            }
            case UpdateOpType::kUnset:
                DeletePathValue(doc, parts);
                break;
            case UpdateOpType::kPush:
            case UpdateOpType::kAddToSet:
            case UpdateOpType::kPull: {
                mimicdb::FieldValue current;
                const bool has_value = GetPathValue(*doc, parts, &current);
                if (!has_value || current.is_null) {
                    if (op.type == UpdateOpType::kPull) {
                        break;
                    }
                    current = mimicdb::FieldValue::Array({});
                }
                if (current.type != mimicdb::FieldType::kArray) {
                    if (error) {
                        *error = "array update requires array field";
                    }
                    return false;
                }
                std::vector<mimicdb::FieldValue> values = current.array;
                if (op.type == UpdateOpType::kPush) {
                    for (const auto& value : op.values) {
                        values.push_back(value);
                    }
                } else if (op.type == UpdateOpType::kAddToSet) {
                    for (const auto& value : op.values) {
                        bool found = false;
                        for (const auto& existing : values) {
                            if (ValuesEqual(existing, value)) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            values.push_back(value);
                        }
                    }
                } else if (op.type == UpdateOpType::kPull) {
                    std::vector<mimicdb::FieldValue> filtered;
                    filtered.reserve(values.size());
                    for (const auto& value : values) {
                        if (!MatchPullValue(value, op)) {
                            filtered.push_back(value);
                        }
                    }
                    values = std::move(filtered);
                }
                if (!SetPathValue(doc, parts,
                                  mimicdb::FieldValue::Array(values),
                                  error)) {
                    return false;
                }
                break;
            }
        }
    }
    return true;
}

MongoDocument BuildUpsertSeed(const std::vector<Filter>& filters,
                              std::string* error) {
    MongoDocument seed;
    for (const auto& filter : filters) {
        if (filter.negated || filter.op != FilterOp::kEq || filter.values.empty()) {
            continue;
        }
        const auto parts = SplitPath(filter.field);
        if (parts.empty()) {
            continue;
        }
        if (!SetPathValue(&seed, parts, filter.values[0], error)) {
            return MongoDocument();
        }
    }
    return seed;
}

bool CompareNumeric(const mimicdb::FieldValue& left, const mimicdb::FieldValue& right,
                    FilterOp op) {
    if (left.is_null || right.is_null) {
        return false;
    }
    double l = 0.0;
    double r = 0.0;
    switch (left.type) {
        case mimicdb::FieldType::kInt32:
            l = left.i32;
            break;
        case mimicdb::FieldType::kInt64:
            l = static_cast<double>(left.i64);
            break;
        case mimicdb::FieldType::kFloat64:
            l = left.f64;
            break;
        case mimicdb::FieldType::kBool:
            l = left.b ? 1.0 : 0.0;
            break;
        case mimicdb::FieldType::kDictInt32:
            l = left.i32;
            break;
        default:
            return false;
    }
    switch (right.type) {
        case mimicdb::FieldType::kInt32:
            r = right.i32;
            break;
        case mimicdb::FieldType::kInt64:
            r = static_cast<double>(right.i64);
            break;
        case mimicdb::FieldType::kFloat64:
            r = right.f64;
            break;
        case mimicdb::FieldType::kBool:
            r = right.b ? 1.0 : 0.0;
            break;
        case mimicdb::FieldType::kDictInt32:
            r = right.i32;
            break;
        default:
            return false;
    }
    switch (op) {
        case FilterOp::kGt:
            return l > r;
        case FilterOp::kLt:
            return l < r;
        case FilterOp::kEq:
            return l == r;
        case FilterOp::kNe:
            return l != r;
        default:
            return false;
    }
}

bool FieldValueToInt64(const mimicdb::FieldValue& value, int64_t* out) {
    if (!out || value.is_null) {
        return false;
    }
    switch (value.type) {
        case mimicdb::FieldType::kInt32:
            *out = static_cast<int64_t>(value.i32);
            return true;
        case mimicdb::FieldType::kInt64:
            *out = value.i64;
            return true;
        case mimicdb::FieldType::kBool:
            *out = value.b ? 1 : 0;
            return true;
        case mimicdb::FieldType::kDictInt32:
            *out = static_cast<int64_t>(value.i32);
            return true;
        default:
            return false;
    }
}

bool ArrayContainsValue(const std::vector<mimicdb::FieldValue>& values,
                        const mimicdb::FieldValue& needle) {
    for (const auto& item : values) {
        if (ValuesEqual(item, needle)) {
            return true;
        }
    }
    return false;
}

bool ArrayContainsAny(const std::vector<mimicdb::FieldValue>& values,
                      const std::vector<mimicdb::FieldValue>& needles) {
    for (const auto& needle : needles) {
        if (ArrayContainsValue(values, needle)) {
            return true;
        }
    }
    return false;
}

bool MatchFilter(const MongoDocument& doc, const Filter& filter) {
    auto it = doc.fields.find(filter.field);
    const bool present = it != doc.fields.end();
    const bool has_value = present && !IsNullValue(it->second);
    bool matched = false;
    if (filter.op == FilterOp::kExists) {
        matched = (filter.exists ? present : !present);
    } else {
        if (!present) {
            matched = false;
        } else if (!has_value) {
            matched = false;
        } else if (it->second.type == mimicdb::FieldType::kArray) {
            const auto& values = it->second.array;
            if (filter.op == FilterOp::kIn || filter.op == FilterOp::kNin) {
                const bool in = ArrayContainsAny(values, filter.values);
                matched = (filter.op == FilterOp::kIn) ? in : !in;
            } else if (filter.op == FilterOp::kAll) {
                bool all_present = true;
                for (const auto& val : filter.values) {
                    if (!ArrayContainsValue(values, val)) {
                        all_present = false;
                        break;
                    }
                }
                matched = all_present;
            } else if (filter.op == FilterOp::kSize) {
                if (filter.values.empty()) {
                    matched = false;
                } else {
                    int64_t size_value = 0;
                    matched = FieldValueToInt64(filter.values[0], &size_value) &&
                              size_value >= 0 &&
                              static_cast<size_t>(size_value) == values.size();
                }
            } else if (filter.op == FilterOp::kRegex) {
                std::regex::flag_type flags = std::regex::ECMAScript;
                if (filter.regex_options.find('i') != std::string::npos) {
                    flags |= std::regex::icase;
                }
                std::regex re(filter.regex, flags);
                bool any_match = false;
                for (const auto& item : values) {
                    if (item.is_null || item.type != mimicdb::FieldType::kString) {
                        continue;
                    }
                    if (std::regex_search(item.bytes, re)) {
                        any_match = true;
                        break;
                    }
                }
                matched = any_match;
            } else if (filter.op == FilterOp::kEq || filter.op == FilterOp::kNe) {
                bool eq = false;
                if (!filter.values.empty()) {
                    if (filter.values[0].type == mimicdb::FieldType::kArray) {
                        eq = ValuesEqual(it->second, filter.values[0]);
                    } else {
                        eq = ArrayContainsValue(values, filter.values[0]);
                    }
                }
                matched = (filter.op == FilterOp::kEq) ? eq : !eq;
            } else {
                matched = false;
            }
        } else if (filter.op == FilterOp::kIn || filter.op == FilterOp::kNin) {
            bool in = false;
            for (const auto& candidate : filter.values) {
                if (ValuesEqual(it->second, candidate)) {
                    in = true;
                    break;
                }
            }
            matched = (filter.op == FilterOp::kIn) ? in : !in;
        } else if (filter.op == FilterOp::kEq || filter.op == FilterOp::kNe) {
            const bool eq = ValuesEqual(it->second, filter.values[0]);
            matched = (filter.op == FilterOp::kEq) ? eq : !eq;
        } else if (filter.op == FilterOp::kAll) {
            if (it->second.type != mimicdb::FieldType::kString ||
                filter.values.empty()) {
                matched = false;
            } else {
                bool all_present = true;
                for (const auto& val : filter.values) {
                    if (!ValuesEqual(it->second, val)) {
                        all_present = false;
                        break;
                    }
                }
                matched = all_present;
            }
        } else if (filter.op == FilterOp::kSize) {
            matched = false;
        } else if (filter.op == FilterOp::kRegex) {
            if (it->second.type != mimicdb::FieldType::kString) {
                matched = false;
            } else {
                std::regex::flag_type flags = std::regex::ECMAScript;
                if (filter.regex_options.find('i') != std::string::npos) {
                    flags |= std::regex::icase;
                }
                std::regex re(filter.regex, flags);
                matched = std::regex_search(it->second.bytes, re);
            }
        } else {
            matched = CompareNumeric(it->second, filter.values[0], filter.op);
        }
    }
    if (filter.negated) {
        matched = !matched;
    }
    return matched;
}

bool MatchFilters(const MongoDocument& doc, const std::vector<Filter>& filters) {
    for (const auto& filter : filters) {
        if (!MatchFilter(doc, filter)) {
            return false;
        }
    }
    return true;
}

bool MatchExpressionDoc(const MongoDocument& doc, const MatchExpression& expr) {
    if (!MatchFilters(doc, expr.filters)) {
        return false;
    }
    switch (expr.op) {
        case MatchOp::kAnd:
            for (const auto& child : expr.children) {
                if (!MatchExpressionDoc(doc, child)) {
                    return false;
                }
            }
            return true;
        case MatchOp::kOr:
            if (expr.children.empty()) {
                return false;
            }
            for (const auto& child : expr.children) {
                if (MatchExpressionDoc(doc, child)) {
                    return true;
                }
            }
            return false;
        case MatchOp::kNor:
            for (const auto& child : expr.children) {
                if (MatchExpressionDoc(doc, child)) {
                    return false;
                }
            }
            return true;
    }
    return false;
}

std::vector<mimicdb::FieldValue> CollectFieldValues(const MongoDocument& doc,
                                                    const std::string& field) {
    std::vector<mimicdb::FieldValue> values;
    auto it = doc.fields.find(field);
    if (it == doc.fields.end() || it->second.is_null) {
        return values;
    }
    if (it->second.type == mimicdb::FieldType::kArray) {
        for (const auto& item : it->second.array) {
            if (!item.is_null) {
                values.push_back(item);
            }
        }
    } else {
        values.push_back(it->second);
    }
    return values;
}

std::vector<MongoDocument> ApplyProjection(const std::vector<MongoDocument>& docs,
                                           const ProjectionSpec& projection) {
    if (projection.include.empty() && projection.exclude.empty() &&
        projection.computed.empty()) {
        return docs;
    }
    std::vector<MongoDocument> out;
    out.reserve(docs.size());
    for (const auto& doc : docs) {
        MongoDocument projected;
        if (!projection.include.empty()) {
            for (const auto& name : projection.include) {
                auto it = doc.fields.find(name);
                if (it != doc.fields.end()) {
                    projected.fields.emplace(name, it->second);
                }
            }
        } else if (!projection.exclude.empty()) {
            for (const auto& item : doc.fields) {
                if (std::find(projection.exclude.begin(), projection.exclude.end(),
                              item.first) == projection.exclude.end()) {
                    projected.fields.emplace(item.first, item.second);
                }
            }
        } else {
            projected = doc;
        }
        for (const auto& computed : projection.computed) {
            if (computed.from_field) {
                auto it = doc.fields.find(computed.field);
                if (it != doc.fields.end()) {
                    projected.fields[computed.name] = it->second;
                }
            } else {
                projected.fields[computed.name] = computed.literal;
            }
        }
        for (const auto& slice : projection.slices) {
            auto it = projected.fields.find(slice.field);
            if (it == projected.fields.end()) {
                continue;
            }
            if (it->second.is_null || it->second.type != mimicdb::FieldType::kArray) {
                continue;
            }
            const auto& values = it->second.array;
            const size_t size = values.size();
            size_t start = 0;
            size_t end = size;
            if (!slice.has_limit) {
                const int32_t count = slice.limit;
                if (count >= 0) {
                    end = static_cast<size_t>(count) > size ? size
                                                            : static_cast<size_t>(count);
                } else {
                    const size_t keep = static_cast<size_t>(-count);
                    start = keep >= size ? 0 : size - keep;
                }
            } else {
                int64_t skip = slice.skip;
                if (skip < 0) {
                    skip = static_cast<int64_t>(size) + skip;
                    if (skip < 0) {
                        skip = 0;
                    }
                }
                const size_t start_index = static_cast<size_t>(skip);
                const size_t limit =
                    slice.limit < 0 ? 0 : static_cast<size_t>(slice.limit);
                start = start_index > size ? size : start_index;
                end = start + limit;
                if (end > size) {
                    end = size;
                }
            }
            std::vector<mimicdb::FieldValue> sliced;
            if (start < end) {
                const auto start_it =
                    values.begin() + static_cast<std::ptrdiff_t>(start);
                const auto end_it =
                    values.begin() + static_cast<std::ptrdiff_t>(end);
                sliced.assign(start_it, end_it);
            }
            it->second = mimicdb::FieldValue::Array(sliced);
        }
        out.push_back(std::move(projected));
    }
    return out;
}

std::string KeyFromValue(const mimicdb::FieldValue& value) {
    if (value.is_null) {
        return "null";
    }
    switch (value.type) {
        case mimicdb::FieldType::kInt32:
            return std::to_string(value.i32);
        case mimicdb::FieldType::kInt64:
            return std::to_string(value.i64);
        case mimicdb::FieldType::kFloat64:
            return std::to_string(value.f64);
        case mimicdb::FieldType::kBool:
            return value.b ? "1" : "0";
        case mimicdb::FieldType::kString:
        case mimicdb::FieldType::kBytes:
            return value.bytes;
        case mimicdb::FieldType::kArray:
            return EncodeArray(value.array);
        case mimicdb::FieldType::kObject: {
            std::vector<std::string> keys;
            keys.reserve(value.object.size());
            for (const auto& item : value.object) {
                keys.push_back(item.first);
            }
            std::sort(keys.begin(), keys.end());
            std::string out;
            for (const auto& key : keys) {
                auto it = value.object.find(key);
                if (it == value.object.end()) {
                    continue;
                }
                out.append(key);
                out.push_back('=');
                out.append(KeyFromValue(it->second));
                out.push_back(';');
            }
            return out;
        }
        case mimicdb::FieldType::kDictInt32:
            return std::to_string(value.i32);
    }
    return {};
}

void AppendUnique(std::vector<std::string>* fields,
                  std::unordered_set<std::string>* seen,
                  const std::string& name) {
    if (!fields || !seen) {
        return;
    }
    if (seen->insert(name).second) {
        fields->push_back(name);
    }
}

void CollectFilterFields(const std::vector<Filter>& filters,
                         std::vector<std::string>* fields,
                         std::unordered_set<std::string>* seen) {
    for (const auto& filter : filters) {
        if (!filter.field.empty()) {
            AppendUnique(fields, seen, filter.field);
        }
    }
}

void CollectMatchFields(const MatchExpression& expr,
                        std::vector<std::string>* fields,
                        std::unordered_set<std::string>* seen) {
    CollectFilterFields(expr.filters, fields, seen);
    for (const auto& child : expr.children) {
        CollectMatchFields(child, fields, seen);
    }
}

bool ProjectionNeedsAll(const ProjectionSpec& projection) {
    if (!projection.exclude.empty()) {
        return true;
    }
    if (projection.include.empty() && projection.computed.empty() && projection.slices.empty()) {
        return true;
    }
    return false;
}

bool IsNumericType(mimicdb::FieldType type) {
    return type == mimicdb::FieldType::kInt32 ||
           type == mimicdb::FieldType::kInt64 ||
           type == mimicdb::FieldType::kFloat64 ||
           type == mimicdb::FieldType::kBool ||
           type == mimicdb::FieldType::kDictInt32;
}

bool TryBuildPredicate(const Filter& filter,
                       const std::unordered_map<std::string, size_t>& field_index,
                       Predicate* out) {
    if (!out) {
        return false;
    }
    if (filter.negated) {
        return false;
    }
    auto it = field_index.find(filter.field);
    if (it == field_index.end()) {
        return false;
    }
    if (filter.op == FilterOp::kExists) {
        out->field_index = it->second;
        out->is_null_check = true;
        out->null_is = !filter.exists;
        return true;
    }
    if (filter.op == FilterOp::kIn || filter.op == FilterOp::kNin ||
        filter.op == FilterOp::kAll || filter.op == FilterOp::kSize ||
        filter.op == FilterOp::kRegex) {
        return false;
    }
    if (filter.values.empty()) {
        return false;
    }
    const auto& value = filter.values[0];
    if (value.is_null) {
        out->field_index = it->second;
        out->is_null_check = true;
        out->null_is = true;
        return true;
    }
    if (value.type == mimicdb::FieldType::kArray ||
        value.type == mimicdb::FieldType::kObject) {
        return false;
    }
    out->field_index = it->second;
    out->value_type = value.type;
    switch (filter.op) {
        case FilterOp::kEq:
            out->op = mimicdb::CompareOp::kEq;
            break;
        case FilterOp::kNe:
            out->op = mimicdb::CompareOp::kNe;
            break;
        case FilterOp::kGt:
            out->op = mimicdb::CompareOp::kGt;
            break;
        case FilterOp::kLt:
            out->op = mimicdb::CompareOp::kLt;
            break;
        default:
            return false;
    }
    if (value.type == mimicdb::FieldType::kString ||
        value.type == mimicdb::FieldType::kBytes) {
        out->bytes = value.bytes;
        return (out->op == mimicdb::CompareOp::kEq ||
                out->op == mimicdb::CompareOp::kNe);
    }
    out->value = 0.0;
    if (value.type == mimicdb::FieldType::kInt32) {
        out->value = value.i32;
    } else if (value.type == mimicdb::FieldType::kInt64) {
        out->value = static_cast<double>(value.i64);
    } else if (value.type == mimicdb::FieldType::kFloat64) {
        out->value = value.f64;
    } else if (value.type == mimicdb::FieldType::kBool) {
        out->value = value.b ? 1.0 : 0.0;
    } else {
        return false;
    }
    return true;
}

bool BuildAggregatePushdown(ApiClientCore* core,
                            const std::vector<FieldDef>& fields,
                            const std::unordered_map<std::string, size_t>& field_index,
                            const std::string& db,
                            const std::string& collection,
                            const PipelineStage& stage,
                            const std::vector<Predicate>& predicates,
                            std::vector<MongoDocument>* out,
                            size_t* rows_scanned_out,
                            std::string* error) {
    if (!out) {
        return false;
    }
    if (rows_scanned_out) {
        *rows_scanned_out = 0;
    }
    out->clear();
    if (stage.type == StageType::kCount) {
        auto id_it = field_index.find("_id");
        if (id_it == field_index.end()) {
            return false;
        }
        const auto& id_field = fields[id_it->second];
        if (!IsNumericType(id_field.type)) {
            return false;
        }
        auto result = core->Aggregate(db, collection, id_it->second, predicates, error);
        if (error && !error->empty()) {
            return false;
        }
        if (rows_scanned_out) {
            *rows_scanned_out += result.rows_scanned;
        }
        MongoDocument doc;
        doc.fields[stage.count_field] = mimicdb::FieldValue::Int64(
            static_cast<int64_t>(result.count));
        out->push_back(std::move(doc));
        return true;
    }
    if (stage.type != StageType::kGroup) {
        return false;
    }
    if (stage.group.has_id || !stage.group.fields.empty() || !stage.group.field.empty()) {
        return false;
    }
    for (const auto& op : stage.ops) {
        if (op.op == "$first" || op.op == "$last") {
            return false;
        }
        if (!op.count_only && op.field.empty()) {
            return false;
        }
    }
    std::unordered_map<size_t, AggregateResult> agg_cache;
    AggregateResult count_result;
    bool count_ready = false;
    auto id_it = field_index.find("_id");
    if (id_it == field_index.end()) {
        return false;
    }
    const auto& id_field = fields[id_it->second];
    if (!IsNumericType(id_field.type)) {
        return false;
    }
    MongoDocument doc;
    doc.fields["_id"] = mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
    for (const auto& op : stage.ops) {
        if (op.count_only) {
            if (!count_ready) {
                count_result = core->Aggregate(db, collection, id_it->second, predicates, error);
                if (error && !error->empty()) {
                    return false;
                }
                if (rows_scanned_out) {
                    *rows_scanned_out += count_result.rows_scanned;
                }
                count_ready = true;
            }
            doc.fields[op.name] = mimicdb::FieldValue::Int64(
                static_cast<int64_t>(count_result.count));
            continue;
        }
        auto field_it = field_index.find(op.field);
        if (field_it == field_index.end()) {
            return false;
        }
        const auto& field = fields[field_it->second];
        if (!IsNumericType(field.type)) {
            return false;
        }
        auto agg_it = agg_cache.find(field_it->second);
        if (agg_it == agg_cache.end()) {
            AggregateResult result =
                core->Aggregate(db, collection, field_it->second, predicates, error);
            if (error && !error->empty()) {
                return false;
            }
            if (rows_scanned_out) {
                *rows_scanned_out += result.rows_scanned;
            }
            agg_it = agg_cache.emplace(field_it->second, result).first;
        }
        const auto& result = agg_it->second;
        if (op.op == "$sum") {
            doc.fields[op.name] = mimicdb::FieldValue::Float64(result.sum);
        } else if (op.op == "$min") {
            if (result.has_value) {
                doc.fields[op.name] = mimicdb::FieldValue::Float64(result.min);
            } else {
                doc.fields[op.name] =
                    mimicdb::FieldValue::Null(mimicdb::FieldType::kFloat64);
            }
        } else if (op.op == "$max") {
            if (result.has_value) {
                doc.fields[op.name] = mimicdb::FieldValue::Float64(result.max);
            } else {
                doc.fields[op.name] =
                    mimicdb::FieldValue::Null(mimicdb::FieldType::kFloat64);
            }
        } else {
            return false;
        }
    }
    out->push_back(std::move(doc));
    return true;
}

bool SplitPushdownFilters(const std::vector<Filter>& filters,
                          const std::unordered_map<std::string, size_t>& field_index,
                          std::vector<Predicate>* out_predicates,
                          std::vector<Filter>* remaining) {
    if (!out_predicates || !remaining) {
        return false;
    }
    out_predicates->clear();
    remaining->clear();
    bool all_supported = true;
    for (const auto& filter : filters) {
        Predicate pred;
        if (TryBuildPredicate(filter, field_index, &pred)) {
            out_predicates->push_back(std::move(pred));
        } else {
            remaining->push_back(filter);
            all_supported = false;
        }
    }
    return all_supported;
}

struct IdFilterInfo {
    bool is_eq = false;
    bool is_in = false;
    int64_t eq_value = 0;
    std::vector<int64_t> in_values;
};

bool ExtractIdFilter(const std::vector<Filter>& filters, IdFilterInfo* out) {
    if (!out || filters.size() != 1) {
        return false;
    }
    const auto& filter = filters[0];
    if (filter.field != "_id" || filter.negated) {
        return false;
    }
    if (filter.op == FilterOp::kEq) {
        if (filter.values.empty() || filter.values[0].is_null) {
            return false;
        }
        if (!FieldValueToInt64(filter.values[0], &out->eq_value)) {
            return false;
        }
        out->is_eq = true;
        return true;
    }
    if (filter.op == FilterOp::kIn) {
        if (filter.values.empty()) {
            return false;
        }
        out->is_in = true;
        out->in_values.clear();
        for (const auto& value : filter.values) {
            int64_t id_value = 0;
            if (!FieldValueToInt64(value, &id_value)) {
                return false;
            }
            out->in_values.push_back(id_value);
        }
        return true;
    }
    return false;
}

std::vector<MongoDocument> LatestDocumentsForIds(ApiClientCore* core,
                                                 const std::unordered_map<std::string, size_t>& field_index,
                                                 const std::string& db,
                                                 const std::string& collection,
                                                 const std::vector<std::string>& columns,
                                                 const IdFilterInfo& id_filter,
                                                 std::string* error) {
    std::vector<MongoDocument> docs;
    std::vector<Predicate> predicates;
    std::unordered_set<int64_t> target_ids;
    if (id_filter.is_eq) {
        auto it = field_index.find("_id");
        if (it != field_index.end()) {
            Predicate pred;
            pred.field_index = it->second;
            pred.op = mimicdb::CompareOp::kEq;
            pred.value = static_cast<double>(id_filter.eq_value);
            pred.value_type = mimicdb::FieldType::kInt64;
            predicates.push_back(pred);
        }
        target_ids.insert(id_filter.eq_value);
    } else if (id_filter.is_in) {
        target_ids.insert(id_filter.in_values.begin(), id_filter.in_values.end());
    }
    ScanResult result = core->Scan(db, collection, columns, predicates, 0, 0, error);
    if (error && !error->empty()) {
        return docs;
    }
    std::unordered_map<int64_t, MongoDocument> latest;
    for (const auto& row : result.rows) {
        MongoDocument doc;
        int64_t doc_id = -1;
        int64_t version = 0;
        bool deleted = false;
        for (size_t i = 0; i < result.columns.size(); ++i) {
            const auto& name = result.columns[i];
            doc.fields[name] = row[i];
            if (name == "_id" && !row[i].is_null) {
                doc_id = row[i].i64;
            } else if (name == "_version" && !row[i].is_null) {
                version = row[i].i64;
            } else if (name == "_deleted" && !row[i].is_null) {
                deleted = row[i].b;
            }
        }
        if (doc_id < 0 || deleted) {
            continue;
        }
        if (!target_ids.empty() && target_ids.find(doc_id) == target_ids.end()) {
            continue;
        }
        auto it = latest.find(doc_id);
        if (it == latest.end()) {
            latest[doc_id] = std::move(doc);
            continue;
        }
        auto existing = it->second.fields.find("_version");
        if (existing == it->second.fields.end() || existing->second.i64 < version) {
            it->second = std::move(doc);
        }
    }
    docs.reserve(latest.size());
    for (auto& item : latest) {
        docs.push_back(std::move(item.second));
    }
    return docs;
}

std::vector<MongoDocument> LatestDocumentsFromScan(ApiClientCore* core,
                                                   const std::unordered_map<std::string, size_t>& field_index,
                                                   const std::string& db,
                                                   const std::string& collection,
                                                   const std::vector<std::string>& columns,
                                                   const std::vector<Predicate>& predicates,
                                                   std::string* error) {
    std::vector<MongoDocument> docs;
    ScanResult result = core->Scan(db, collection, columns, predicates, 0, 0, error);
    if (error && !error->empty()) {
        return docs;
    }
    std::unordered_map<int64_t, MongoDocument> latest;
    for (const auto& row : result.rows) {
        MongoDocument doc;
        int64_t doc_id = -1;
        int64_t version = 0;
        bool deleted = false;
        for (size_t i = 0; i < result.columns.size(); ++i) {
            const auto& name = result.columns[i];
            doc.fields[name] = row[i];
            if (name == "_id" && !row[i].is_null) {
                doc_id = row[i].i64;
            } else if (name == "_version" && !row[i].is_null) {
                version = row[i].i64;
            } else if (name == "_deleted" && !row[i].is_null) {
                deleted = row[i].b;
            }
        }
        if (doc_id < 0 || deleted) {
            continue;
        }
        auto it = latest.find(doc_id);
        if (it == latest.end()) {
            latest[doc_id] = std::move(doc);
            continue;
        }
        auto existing = it->second.fields.find("_version");
        if (existing == it->second.fields.end() || existing->second.i64 < version) {
            it->second = std::move(doc);
        }
    }
    docs.reserve(latest.size());
    for (auto& item : latest) {
        docs.push_back(std::move(item.second));
    }
    return docs;
}

int CompareFieldValues(const mimicdb::FieldValue& left, const mimicdb::FieldValue& right) {
    if (left.is_null && right.is_null) {
        return 0;
    }
    if (left.is_null) {
        return 1;
    }
    if (right.is_null) {
        return -1;
    }
    if (left.type != right.type) {
        return static_cast<int>(left.type) < static_cast<int>(right.type) ? -1 : 1;
    }
    switch (left.type) {
        case mimicdb::FieldType::kInt32:
            return left.i32 < right.i32 ? -1 : (left.i32 > right.i32 ? 1 : 0);
        case mimicdb::FieldType::kInt64:
            return left.i64 < right.i64 ? -1 : (left.i64 > right.i64 ? 1 : 0);
        case mimicdb::FieldType::kFloat64:
            return left.f64 < right.f64 ? -1 : (left.f64 > right.f64 ? 1 : 0);
        case mimicdb::FieldType::kBool:
            return left.b == right.b ? 0 : (left.b ? 1 : -1);
        case mimicdb::FieldType::kString:
        case mimicdb::FieldType::kBytes:
            return left.bytes < right.bytes ? -1 : (left.bytes > right.bytes ? 1 : 0);
        case mimicdb::FieldType::kArray: {
            const std::string left_key = EncodeArray(left.array);
            const std::string right_key = EncodeArray(right.array);
            return left_key < right_key ? -1 : (left_key > right_key ? 1 : 0);
        }
        case mimicdb::FieldType::kObject: {
            const std::string left_key = KeyFromValue(left);
            const std::string right_key = KeyFromValue(right);
            return left_key < right_key ? -1 : (left_key > right_key ? 1 : 0);
        }
        case mimicdb::FieldType::kDictInt32:
            return left.i32 < right.i32 ? -1 : (left.i32 > right.i32 ? 1 : 0);
    }
    return 0;
}

}  // namespace

MongoClientCore::MongoClientCore(ApiClientCore* core) : core_(core) {}

MongoClientCore::CollectionState* MongoClientCore::GetCollection(
    const std::string& db, const std::string& collection) {
    auto db_it = collections_.find(db);
    if (db_it == collections_.end()) {
        return nullptr;
    }
    auto it = db_it->second.find(collection);
    if (it == db_it->second.end()) {
        return nullptr;
    }
    return &it->second;
}

const MongoClientCore::CollectionState* MongoClientCore::GetCollection(
    const std::string& db, const std::string& collection) const {
    auto db_it = collections_.find(db);
    if (db_it == collections_.end()) {
        return nullptr;
    }
    auto it = db_it->second.find(collection);
    if (it == db_it->second.end()) {
        return nullptr;
    }
    return &it->second;
}

bool MongoClientCore::EnsureSchema(const std::string& db, const std::string& collection,
                                   const std::vector<MongoDocument>& docs,
                                   std::string* error) {
    auto& db_state = collections_[db];
    auto& state = db_state[collection];
    if (state.initialized) {
        if (core_->FieldsFor(db, collection) != nullptr) {
            return true;
        }
        state = CollectionState();
    }
    state.fields = {
        {"_id", mimicdb::FieldType::kInt64},
        {"_deleted", mimicdb::FieldType::kBool},
        {"_version", mimicdb::FieldType::kInt64},
    };
    for (const auto& doc : docs) {
        for (const auto& item : doc.fields) {
            const auto& name = item.first;
            if (name == "_id") {
                continue;
            }
            if (!IsScalar(item.second.type)) {
                if (error) {
                    *error = "unsupported field type";
                }
                return false;
            }
            if (state.field_index.find(name) != state.field_index.end()) {
                continue;
            }
            state.field_index[name] = state.fields.size();
            state.fields.push_back(InferField(name, item.second));
        }
    }
    for (size_t i = 0; i < state.fields.size(); ++i) {
        state.field_index[state.fields[i].name] = i;
    }
    core_->CreateDatabase(db);
    if (!core_->CreateDataset(db, collection, state.fields)) {
        if (error) {
            *error = "create_dataset failed";
        }
        return false;
    }
    state.latest_cache.clear();
    state.last_seen_version = 0;
    state.cache_valid = false;
    state.append_only = true;
    state.initialized = true;
    return true;
}

uint64_t MongoClientCore::NextVersion() {
    version_counter_ += 1;
    return (static_cast<uint64_t>(std::time(nullptr)) << 32) ^ version_counter_;
}

bool MongoClientCore::InsertMany(const std::string& db, const std::string& collection,
                                 const std::vector<MongoDocument>& docs,
                                 std::string* error) {
    if (!EnsureSchema(db, collection, docs, error)) {
        return false;
    }
    auto* state = GetCollection(db, collection);
    if (!state) {
        if (error) {
            *error = "unknown collection";
        }
        return false;
    }
    state->batch_scratch.clear();
    state->i64_scratch.clear();
    state->i32_scratch.clear();
    state->f64_scratch.clear();
    state->bool_scratch.clear();
    state->length_scratch.clear();
    state->bytes_scratch.clear();
    state->validity_scratch.clear();

    const size_t count = docs.size();
    for (const auto& field : state->fields) {
        mimicdb::FieldBatch batch;
        batch.type = field.type;
        batch.count = count;
        std::vector<uint8_t> validity;
        validity.reserve(count);
        if (field.name == "_id") {
            std::vector<int64_t> values;
            values.reserve(count);
            for (const auto& doc : docs) {
                auto it = doc.fields.find("_id");
                int64_t id = 0;
                if (it == doc.fields.end() || it->second.is_null) {
                    id = state->next_id++;
                } else {
                    id = it->second.i64;
                }
                values.push_back(id);
                validity.push_back(1);
            }
            state->i64_scratch.push_back(std::move(values));
            batch.data = state->i64_scratch.back().data();
        } else if (field.name == "_version") {
            std::vector<int64_t> values;
            values.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                values.push_back(static_cast<int64_t>(NextVersion()));
                validity.push_back(1);
            }
            state->i64_scratch.push_back(std::move(values));
            batch.data = state->i64_scratch.back().data();
        } else if (field.name == "_deleted") {
            std::vector<uint8_t> values;
            values.reserve(count);
            for (const auto& doc : docs) {
                auto it = doc.fields.find("_deleted");
                if (it == doc.fields.end() || it->second.is_null) {
                    values.push_back(0);
                } else {
                    values.push_back(it->second.b ? 1 : 0);
                }
                validity.push_back(1);
            }
            state->bool_scratch.push_back(std::move(values));
            batch.data = state->bool_scratch.back().data();
        } else if (field.type == mimicdb::FieldType::kInt64) {
            std::vector<int64_t> values;
            values.reserve(count);
            for (const auto& doc : docs) {
                auto it = doc.fields.find(field.name);
                if (it == doc.fields.end() || it->second.is_null) {
                    values.push_back(0);
                    validity.push_back(0);
                } else {
                    values.push_back(it->second.i64);
                    validity.push_back(1);
                }
            }
            state->i64_scratch.push_back(std::move(values));
            batch.data = state->i64_scratch.back().data();
        } else if (field.type == mimicdb::FieldType::kInt32) {
            std::vector<int32_t> values;
            values.reserve(count);
            for (const auto& doc : docs) {
                auto it = doc.fields.find(field.name);
                if (it == doc.fields.end() || it->second.is_null) {
                    values.push_back(0);
                    validity.push_back(0);
                } else {
                    values.push_back(it->second.i32);
                    validity.push_back(1);
                }
            }
            state->i32_scratch.push_back(std::move(values));
            batch.data = state->i32_scratch.back().data();
        } else if (field.type == mimicdb::FieldType::kFloat64) {
            std::vector<double> values;
            values.reserve(count);
            for (const auto& doc : docs) {
                auto it = doc.fields.find(field.name);
                if (it == doc.fields.end() || it->second.is_null) {
                    values.push_back(0.0);
                    validity.push_back(0);
                } else {
                    values.push_back(it->second.f64);
                    validity.push_back(1);
                }
            }
            state->f64_scratch.push_back(std::move(values));
            batch.data = state->f64_scratch.back().data();
        } else if (field.type == mimicdb::FieldType::kBool) {
            std::vector<uint8_t> values;
            values.reserve(count);
            for (const auto& doc : docs) {
                auto it = doc.fields.find(field.name);
                if (it == doc.fields.end() || it->second.is_null) {
                    values.push_back(0);
                    validity.push_back(0);
                } else {
                    values.push_back(it->second.b ? 1 : 0);
                    validity.push_back(1);
                }
            }
            state->bool_scratch.push_back(std::move(values));
            batch.data = state->bool_scratch.back().data();
        } else if (field.type == mimicdb::FieldType::kString ||
                   field.type == mimicdb::FieldType::kBytes ||
                   field.type == mimicdb::FieldType::kArray) {
            std::vector<uint32_t> lengths;
            std::vector<uint8_t> bytes;
            lengths.reserve(count);
            for (const auto& doc : docs) {
                auto it = doc.fields.find(field.name);
                if (it == doc.fields.end() || it->second.is_null) {
                    lengths.push_back(0);
                    validity.push_back(0);
                } else {
                    const std::string data = (field.type == mimicdb::FieldType::kArray)
                                                 ? EncodeArray(it->second.array)
                                                 : it->second.bytes;
                    lengths.push_back(static_cast<uint32_t>(data.size()));
                    bytes.insert(bytes.end(), data.begin(), data.end());
                    validity.push_back(1);
                }
            }
            state->length_scratch.push_back(std::move(lengths));
            state->bytes_scratch.push_back(std::move(bytes));
            batch.lengths = state->length_scratch.back().data();
            batch.bytes = state->bytes_scratch.back().data();
            batch.bytes_size = state->bytes_scratch.back().size();
        }
        state->validity_scratch.push_back(std::move(validity));
        batch.validity = state->validity_scratch.back().data();
        state->batch_scratch.push_back(batch);
    }
    return core_->AppendBatch(db, collection, state->batch_scratch, error);
}

std::vector<MongoDocument> MongoClientCore::Find(const std::string& db,
                                                 const std::string& collection,
                                                 const std::vector<Filter>& filters,
                                                 const ProjectionSpec& projection,
                                                 const FindOptions& options,
                                                 std::string* error) const {
    std::vector<MongoDocument> out;
    const auto* state = GetCollection(db, collection);
    if (!state) {
        if (error) {
            *error = "unknown collection";
        }
        return out;
    }
    std::vector<std::string> required_fields;
    std::unordered_set<std::string> seen_fields;
    CollectFilterFields(filters, &required_fields, &seen_fields);
    for (const auto& spec : options.sort) {
        AppendUnique(&required_fields, &seen_fields, spec.field);
    }
    if (!ProjectionNeedsAll(projection)) {
        for (const auto& name : projection.include) {
            AppendUnique(&required_fields, &seen_fields, name);
        }
        for (const auto& computed : projection.computed) {
            if (computed.from_field) {
                AppendUnique(&required_fields, &seen_fields, computed.field);
            }
        }
        for (const auto& slice : projection.slices) {
            AppendUnique(&required_fields, &seen_fields, slice.field);
        }
    } else {
        required_fields.clear();
    }

    std::vector<Predicate> pushdown;
    std::vector<Filter> remaining;
    SplitPushdownFilters(filters, state->field_index, &pushdown, &remaining);
    const bool use_pushdown = !pushdown.empty() && remaining.empty();
    IdFilterInfo id_filter;
    const bool use_id_fast_path = ExtractIdFilter(filters, &id_filter);
    std::vector<std::string> scan_columns;
    if (required_fields.empty()) {
        for (const auto& field : state->fields) {
            scan_columns.push_back(field.name);
        }
    } else {
        scan_columns = required_fields;
        AppendUnique(&scan_columns, &seen_fields, "_id");
        AppendUnique(&scan_columns, &seen_fields, "_version");
        AppendUnique(&scan_columns, &seen_fields, "_deleted");
    }

    auto docs = use_id_fast_path
        ? LatestDocumentsForIds(core_, state->field_index, db, collection,
                                scan_columns, id_filter, error)
        : use_pushdown
            ? LatestDocumentsFromScan(core_, state->field_index, db, collection,
                                      scan_columns, pushdown, error)
            : LatestDocuments(db, collection,
                              required_fields.empty() ? std::vector<std::string>() : required_fields,
                              error);
    if (error && !error->empty()) {
        return out;
    }
    for (const auto& doc : docs) {
        if (!use_pushdown && !use_id_fast_path && !MatchFilters(doc, filters)) {
            continue;
        }
        out.push_back(doc);
    }
    if (!options.sort.empty()) {
        std::stable_sort(out.begin(), out.end(),
                         [&](const MongoDocument& left, const MongoDocument& right) {
                             for (const auto& spec : options.sort) {
                                 mimicdb::FieldValue left_value =
                                     mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
                                 mimicdb::FieldValue right_value =
                                     mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
                                 auto lit = left.fields.find(spec.field);
                                 if (lit != left.fields.end()) {
                                     left_value = lit->second;
                                 }
                                 auto rit = right.fields.find(spec.field);
                                 if (rit != right.fields.end()) {
                                     right_value = rit->second;
                                 }
                                 const int cmp = CompareFieldValues(left_value, right_value);
                                 if (cmp == 0) {
                                     continue;
                                 }
                                 return spec.direction < 0 ? (cmp > 0) : (cmp < 0);
                             }
                             return false;
                         });
    }
    if (options.skip > 0 && options.skip < out.size()) {
        out.erase(out.begin(),
                  out.begin() + static_cast<std::ptrdiff_t>(options.skip));
    } else if (options.skip >= out.size()) {
        out.clear();
    }
    if (options.limit > 0 && options.limit < out.size()) {
        out.resize(options.limit);
    }
    return ApplyProjection(out, projection);
}

std::vector<MongoDocument> MongoClientCore::AggregatePipeline(
    const std::string& db, const std::string& collection,
    const std::vector<PipelineStage>& pipeline,
    std::string* error) const {
    std::vector<MongoDocument> out;
    const auto* state = GetCollection(db, collection);
    if (!state) {
        if (error) {
            *error = "unknown collection";
        }
        return out;
    }
    if (!pipeline.empty()) {
        const PipelineStage* match_stage = nullptr;
        const PipelineStage* agg_stage = nullptr;
        std::vector<Predicate> match_predicates;
        if (pipeline.size() == 1) {
            agg_stage = &pipeline[0];
        } else if (pipeline.size() == 2 && pipeline[0].type == StageType::kMatch) {
            match_stage = &pipeline[0];
            agg_stage = &pipeline[1];
        }
        bool pushdown_ok = agg_stage &&
            (agg_stage->type == StageType::kGroup || agg_stage->type == StageType::kCount);
        if (pushdown_ok && match_stage) {
            if (match_stage->match.op == MatchOp::kAnd && match_stage->match.children.empty()) {
                std::vector<Filter> remaining;
                if (SplitPushdownFilters(match_stage->match.filters, state->field_index,
                                         &match_predicates, &remaining) &&
                    remaining.empty()) {
                    pushdown_ok = true;
                } else {
                    pushdown_ok = false;
                }
            } else {
                pushdown_ok = false;
            }
        }
        if (pushdown_ok && agg_stage && state->append_only &&
            (!match_stage || !match_predicates.empty() ||
             match_stage->match.filters.empty())) {
            std::vector<MongoDocument> agg_out;
            size_t rows_scanned = 0;
            if (BuildAggregatePushdown(core_, state->fields, state->field_index,
                                       db, collection, *agg_stage,
                                       match_predicates, &agg_out,
                                       &rows_scanned, error)) {
                state->stats.last_scan_rows = rows_scanned;
                state->stats.last_returned_rows = agg_out.size();
                state->stats.last_full_rebuild = false;
                return agg_out;
            }
        }
    }
    std::vector<std::string> required_fields;
    std::unordered_set<std::string> seen_fields;
    bool needs_all = false;
    for (const auto& stage : pipeline) {
        if (stage.type == StageType::kLookup || stage.type == StageType::kFacet) {
            needs_all = true;
            break;
        }
        if (stage.type == StageType::kMatch) {
            CollectMatchFields(stage.match, &required_fields, &seen_fields);
        } else if (stage.type == StageType::kGroup) {
            if (stage.group.has_id) {
                if (!stage.group.fields.empty()) {
                    for (const auto& field : stage.group.fields) {
                        AppendUnique(&required_fields, &seen_fields, field.field);
                    }
                } else {
                    AppendUnique(&required_fields, &seen_fields, stage.group.field);
                }
            }
            for (const auto& op : stage.ops) {
                if (!op.field.empty()) {
                    AppendUnique(&required_fields, &seen_fields, op.field);
                }
            }
        } else if (stage.type == StageType::kSortByCount) {
            if (stage.sort_by_count.is_field) {
                AppendUnique(&required_fields, &seen_fields, stage.sort_by_count.field);
            }
        } else if (stage.type == StageType::kAddFields) {
            for (const auto& field : stage.add_fields) {
                if (field.from_field) {
                    AppendUnique(&required_fields, &seen_fields, field.field);
                }
            }
        } else if (stage.type == StageType::kProject) {
            if (ProjectionNeedsAll(stage.project)) {
                needs_all = true;
                break;
            }
            for (const auto& name : stage.project.include) {
                AppendUnique(&required_fields, &seen_fields, name);
            }
            for (const auto& field : stage.project.computed) {
                if (field.from_field) {
                    AppendUnique(&required_fields, &seen_fields, field.field);
                }
            }
        } else if (stage.type == StageType::kUnwind) {
            AppendUnique(&required_fields, &seen_fields, stage.unwind.field);
        }
    }

    bool match_pushdown = false;
    std::vector<Predicate> match_predicates;
    if (!pipeline.empty() && pipeline[0].type == StageType::kMatch &&
        pipeline[0].match.op == MatchOp::kAnd &&
        pipeline[0].match.children.empty()) {
        std::vector<Filter> remaining;
        if (SplitPushdownFilters(pipeline[0].match.filters, state->field_index,
                                 &match_predicates, &remaining) &&
            remaining.empty() && !match_predicates.empty()) {
            match_pushdown = true;
        }
    }

    std::vector<std::string> scan_columns;
    if (needs_all) {
        for (const auto& field : state->fields) {
            scan_columns.push_back(field.name);
        }
    } else {
        scan_columns = required_fields;
        AppendUnique(&scan_columns, &seen_fields, "_id");
        AppendUnique(&scan_columns, &seen_fields, "_version");
        AppendUnique(&scan_columns, &seen_fields, "_deleted");
    }

    auto docs = match_pushdown
        ? LatestDocumentsFromScan(core_, state->field_index, db, collection, scan_columns,
                                  match_predicates, error)
        : LatestDocuments(db, collection,
                          needs_all ? std::vector<std::string>() : required_fields,
                          error);
    if (error && !error->empty()) {
        return out;
    }

    auto eval_computed = [](const MongoDocument& doc, const ComputedField& field) {
        if (field.from_field) {
            auto it = doc.fields.find(field.field);
            if (it == doc.fields.end()) {
                return mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
            }
            return it->second;
        }
        return field.literal;
    };

    auto object_from_doc = [](const MongoDocument& doc) {
        return mimicdb::FieldValue::Object(doc.fields);
    };

    auto array_from_docs = [&](const std::vector<MongoDocument>& docs_in) {
        std::vector<mimicdb::FieldValue> items;
        items.reserve(docs_in.size());
        for (const auto& doc : docs_in) {
            items.push_back(object_from_doc(doc));
        }
        return mimicdb::FieldValue::Array(items);
    };

    auto group_docs = [&](const std::vector<MongoDocument>& input,
                          const GroupBySpec& group,
                          const std::vector<AggregateOp>& ops_in) {
        struct OpState {
            double sum = 0.0;
            double min = 0.0;
            double max = 0.0;
            bool has_minmax = false;
            uint64_t count = 0;
            mimicdb::FieldValue first;
            bool has_first = false;
            mimicdb::FieldValue last;
            bool has_last = false;
        };
        struct AggState {
            mimicdb::FieldValue id;
            std::unordered_map<std::string, OpState> ops;
        };
        std::unordered_map<std::string, AggState> groups;

        auto to_numeric = [](const mimicdb::FieldValue& value, double* out_value) {
            if (!out_value || value.is_null) {
                return false;
            }
            switch (value.type) {
                case mimicdb::FieldType::kInt32:
                    *out_value = value.i32;
                    return true;
                case mimicdb::FieldType::kInt64:
                    *out_value = static_cast<double>(value.i64);
                    return true;
                case mimicdb::FieldType::kFloat64:
                    *out_value = value.f64;
                    return true;
                case mimicdb::FieldType::kBool:
                    *out_value = value.b ? 1.0 : 0.0;
                    return true;
                case mimicdb::FieldType::kDictInt32:
                    *out_value = value.i32;
                    return true;
                default:
                    return false;
            }
        };

        for (const auto& doc : input) {
            mimicdb::FieldValue id_value;
            if (!group.has_id) {
                id_value = mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
            } else if (!group.fields.empty()) {
                std::unordered_map<std::string, mimicdb::FieldValue> object;
                object.reserve(group.fields.size());
                for (const auto& field : group.fields) {
                    auto it = doc.fields.find(field.field);
                    if (it == doc.fields.end()) {
                        object[field.name] =
                            mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
                    } else {
                        object[field.name] = it->second;
                    }
                }
                id_value = mimicdb::FieldValue::Object(object);
            } else {
                auto it = doc.fields.find(group.field);
                if (it == doc.fields.end()) {
                    id_value = mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
                } else {
                    id_value = it->second;
                }
            }

            const std::string key = KeyFromValue(id_value);
            auto& state = groups[key];
            state.id = id_value;
            for (const auto& op : ops_in) {
                auto& op_state = state.ops[op.name];
                if (op.count_only) {
                    op_state.count += 1;
                    continue;
                }
                auto it = doc.fields.find(op.field);
                if (it == doc.fields.end() || it->second.is_null) {
                    continue;
                }
                if (op.op == "$first") {
                    if (!op_state.has_first) {
                        op_state.first = it->second;
                        op_state.has_first = true;
                    }
                    continue;
                }
                if (op.op == "$last") {
                    op_state.last = it->second;
                    op_state.has_last = true;
                    continue;
                }
                double value = 0.0;
                if (!to_numeric(it->second, &value)) {
                    continue;
                }
                if (op.op == "$sum") {
                    op_state.sum += value;
                } else if (op.op == "$min") {
                    if (!op_state.has_minmax) {
                        op_state.min = value;
                        op_state.max = value;
                        op_state.has_minmax = true;
                    } else {
                        op_state.min = std::min(op_state.min, value);
                    }
                } else if (op.op == "$max") {
                    if (!op_state.has_minmax) {
                        op_state.min = value;
                        op_state.max = value;
                        op_state.has_minmax = true;
                    } else {
                        op_state.max = std::max(op_state.max, value);
                    }
                }
            }
        }

        std::vector<MongoDocument> grouped;
        grouped.reserve(groups.size());
        for (const auto& item : groups) {
            MongoDocument doc;
            doc.fields["_id"] = item.second.id;
            for (const auto& op : ops_in) {
                const auto it = item.second.ops.find(op.name);
                if (it == item.second.ops.end()) {
                    doc.fields[op.name] =
                        mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
                    continue;
                }
                const auto& op_state = it->second;
                if (op.count_only) {
                    doc.fields[op.name] = mimicdb::FieldValue::Int64(
                        static_cast<int64_t>(op_state.count));
                } else if (op.op == "$sum") {
                    doc.fields[op.name] = mimicdb::FieldValue::Float64(op_state.sum);
                } else if (op.op == "$min") {
                    if (op_state.has_minmax) {
                        doc.fields[op.name] = mimicdb::FieldValue::Float64(op_state.min);
                    } else {
                        doc.fields[op.name] =
                            mimicdb::FieldValue::Null(mimicdb::FieldType::kFloat64);
                    }
                } else if (op.op == "$max") {
                    if (op_state.has_minmax) {
                        doc.fields[op.name] = mimicdb::FieldValue::Float64(op_state.max);
                    } else {
                        doc.fields[op.name] =
                            mimicdb::FieldValue::Null(mimicdb::FieldType::kFloat64);
                    }
                } else if (op.op == "$first") {
                    if (op_state.has_first) {
                        doc.fields[op.name] = op_state.first;
                    } else {
                        doc.fields[op.name] =
                            mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
                    }
                } else if (op.op == "$last") {
                    if (op_state.has_last) {
                        doc.fields[op.name] = op_state.last;
                    } else {
                        doc.fields[op.name] =
                            mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
                    }
                }
            }
            grouped.push_back(std::move(doc));
        }
        return grouped;
    };

    std::function<std::vector<MongoDocument>(std::vector<MongoDocument>,
                                             const std::vector<PipelineStage>&)>
        execute_pipeline;

    execute_pipeline =
        [&](std::vector<MongoDocument> current,
            const std::vector<PipelineStage>& stages) -> std::vector<MongoDocument> {
        for (const auto& stage : stages) {
            switch (stage.type) {
                case StageType::kMatch: {
                    std::vector<MongoDocument> filtered;
                    filtered.reserve(current.size());
                    for (const auto& doc : current) {
                        if (MatchExpressionDoc(doc, stage.match)) {
                            filtered.push_back(doc);
                        }
                    }
                    current = std::move(filtered);
                    break;
                }
                case StageType::kGroup:
                    current = group_docs(current, stage.group, stage.ops);
                    break;
                case StageType::kCount: {
                    MongoDocument doc;
                    doc.fields[stage.count_field] = mimicdb::FieldValue::Int64(
                        static_cast<int64_t>(current.size()));
                    current.clear();
                    current.push_back(std::move(doc));
                    break;
                }
                case StageType::kSortByCount: {
                    std::unordered_map<std::string,
                                       std::pair<mimicdb::FieldValue, uint64_t>>
                        counts;
                    for (const auto& doc : current) {
                        mimicdb::FieldValue key_value;
                        if (stage.sort_by_count.is_field) {
                            auto it = doc.fields.find(stage.sort_by_count.field);
                            if (it == doc.fields.end()) {
                                key_value =
                                    mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
                            } else {
                                key_value = it->second;
                            }
                        } else {
                            key_value = stage.sort_by_count.literal;
                        }
                        const std::string key = KeyFromValue(key_value);
                        auto& entry = counts[key];
                        entry.first = key_value;
                        entry.second += 1;
                    }
                    std::vector<std::pair<mimicdb::FieldValue, uint64_t>> buckets;
                    buckets.reserve(counts.size());
                    for (const auto& item : counts) {
                        buckets.push_back(item.second);
                    }
                    std::sort(buckets.begin(), buckets.end(),
                              [](const auto& left, const auto& right) {
                                  return left.second > right.second;
                              });
                    std::vector<MongoDocument> bucket_docs;
                    bucket_docs.reserve(buckets.size());
                    for (const auto& bucket : buckets) {
                        MongoDocument doc;
                        doc.fields["_id"] = bucket.first;
                        doc.fields["count"] = mimicdb::FieldValue::Int64(
                            static_cast<int64_t>(bucket.second));
                        bucket_docs.push_back(std::move(doc));
                    }
                    current = std::move(bucket_docs);
                    break;
                }
                case StageType::kAddFields: {
                    std::vector<MongoDocument> updated;
                    updated.reserve(current.size());
                    for (const auto& doc : current) {
                        MongoDocument out_doc = doc;
                        for (const auto& field : stage.add_fields) {
                            out_doc.fields[field.name] = eval_computed(doc, field);
                        }
                        updated.push_back(std::move(out_doc));
                    }
                    current = std::move(updated);
                    break;
                }
                case StageType::kProject: {
                    std::vector<MongoDocument> projected;
                    projected.reserve(current.size());
                    const bool has_computed = !stage.project.computed.empty();
                    const bool has_include = !stage.project.include.empty();
                    const bool has_exclude = !stage.project.exclude.empty();
                    for (const auto& doc : current) {
                        MongoDocument out_doc;
                        if (has_include && !has_computed && !has_exclude) {
                            for (const auto& name : stage.project.include) {
                                auto it = doc.fields.find(name);
                                if (it != doc.fields.end()) {
                                    out_doc.fields[name] = it->second;
                                }
                            }
                        } else if (has_exclude && !has_computed && !has_include) {
                            for (const auto& item : doc.fields) {
                                if (std::find(stage.project.exclude.begin(),
                                              stage.project.exclude.end(),
                                              item.first) ==
                                    stage.project.exclude.end()) {
                                    out_doc.fields[item.first] = item.second;
                                }
                            }
                        } else {
                            if (has_include) {
                                for (const auto& name : stage.project.include) {
                                    auto it = doc.fields.find(name);
                                    if (it != doc.fields.end()) {
                                        out_doc.fields[name] = it->second;
                                    }
                                }
                            }
                            for (const auto& field : stage.project.computed) {
                                out_doc.fields[field.name] = eval_computed(doc, field);
                            }
                        }
                        projected.push_back(std::move(out_doc));
                    }
                    current = std::move(projected);
                    break;
                }
                case StageType::kUnwind: {
                    std::vector<MongoDocument> unwound;
                    for (const auto& doc : current) {
                        auto it = doc.fields.find(stage.unwind.field);
                        if (it == doc.fields.end()) {
                            if (stage.unwind.preserve_null) {
                                unwound.push_back(doc);
                            }
                            continue;
                        }
                        const auto& value = it->second;
                        if (value.type != mimicdb::FieldType::kArray) {
                            MongoDocument out_doc = doc;
                            out_doc.fields[stage.unwind.field] = value;
                            unwound.push_back(std::move(out_doc));
                            continue;
                        }
                        if (value.array.empty()) {
                            if (stage.unwind.preserve_null) {
                                unwound.push_back(doc);
                            }
                            continue;
                        }
                        for (const auto& item : value.array) {
                            MongoDocument out_doc = doc;
                            out_doc.fields[stage.unwind.field] = item;
                            unwound.push_back(std::move(out_doc));
                        }
                    }
                    current = std::move(unwound);
                    break;
                }
                case StageType::kLookup: {
                    auto foreign_docs = LatestDocuments(db, stage.lookup.from, error);
                    if (error && !error->empty()) {
                        return {};
                    }
                    std::vector<MongoDocument> joined;
                    joined.reserve(current.size());
                    for (const auto& doc : current) {
                        const auto local_values =
                            CollectFieldValues(doc, stage.lookup.local_field);
                        std::vector<MongoDocument> matches;
                        if (!local_values.empty()) {
                            for (const auto& foreign : foreign_docs) {
                                auto fit =
                                    foreign.fields.find(stage.lookup.foreign_field);
                                if (fit == foreign.fields.end() || fit->second.is_null) {
                                    continue;
                                }
                                bool hit = false;
                                for (const auto& local_value : local_values) {
                                    if (ValuesEqual(local_value, fit->second)) {
                                        hit = true;
                                        break;
                                    }
                                }
                                if (hit) {
                                    matches.push_back(foreign);
                                }
                            }
                        }
                        MongoDocument out_doc = doc;
                        out_doc.fields[stage.lookup.as_field] = array_from_docs(matches);
                        joined.push_back(std::move(out_doc));
                    }
                    current = std::move(joined);
                    break;
                }
                case StageType::kFacet: {
                    MongoDocument out_doc;
                    for (const auto& branch : stage.facet.branches) {
                        const auto branch_docs =
                            execute_pipeline(current, branch.second);
                        out_doc.fields[branch.first] = array_from_docs(branch_docs);
                    }
                    current.clear();
                    current.push_back(std::move(out_doc));
                    break;
                }
            }
        }
        return current;
    };

    if (match_pushdown && pipeline.size() > 1) {
        std::vector<PipelineStage> remaining_stages(pipeline.begin() + 1, pipeline.end());
        out = execute_pipeline(std::move(docs), remaining_stages);
    } else if (match_pushdown) {
        out = std::move(docs);
    } else {
        out = execute_pipeline(std::move(docs), pipeline);
    }
    return out;
}

size_t MongoClientCore::Update(const std::string& db, const std::string& collection,
                               const std::vector<Filter>& filters,
                               const UpdateSpec& update,
                               bool multi, bool upsert, bool replace,
                               std::string* error) {
    auto* state = GetCollection(db, collection);
    if (!state) {
        if (error) {
            *error = "unknown collection";
        }
        return 0;
    }
    if (!UpdateCache(db, collection, {}, error)) {
        return 0;
    }
    IdFilterInfo id_filter;
    std::vector<std::string> scan_columns;
    bool use_id_fast_path = ExtractIdFilter(filters, &id_filter);
    if (use_id_fast_path) {
        for (const auto& field : state->fields) {
            scan_columns.push_back(field.name);
        }
    }
    state->append_only = false;
    std::vector<MongoDocument> updates_out;
    size_t matched = 0;
    for (const auto& item : state->latest_cache) {
        if (use_id_fast_path) {
            if (id_filter.is_eq && item.first != id_filter.eq_value) {
                continue;
            }
            if (id_filter.is_in &&
                std::find(id_filter.in_values.begin(), id_filter.in_values.end(),
                          item.first) == id_filter.in_values.end()) {
                continue;
            }
        } else if (!MatchFilters(item.second, filters)) {
            continue;
        }
        matched += 1;
        MongoDocument updated;
        if (replace || update.is_replacement) {
            updated.fields = update.replacement;
            updated.fields["_id"] = mimicdb::FieldValue::Int64(item.first);
        } else {
            updated = item.second;
            if (!ApplyUpdateOps(&updated, update, false, error)) {
                return matched;
            }
        }
        updated.fields["_deleted"] = mimicdb::FieldValue::Bool(false);
        updates_out.push_back(std::move(updated));
        if (!multi) {
            break;
        }
    }
    if (updates_out.empty() && upsert) {
        MongoDocument seed = BuildUpsertSeed(filters, error);
        if (error && !error->empty()) {
            return 0;
        }
        if (replace || update.is_replacement) {
            MongoDocument replacement;
            replacement.fields = update.replacement;
            auto id_it = seed.fields.find("_id");
            if (id_it != seed.fields.end()) {
                replacement.fields["_id"] = id_it->second;
            }
            seed = std::move(replacement);
        }
        if (!replace && !update.is_replacement) {
            if (!ApplyUpdateOps(&seed, update, true, error)) {
                return 0;
            }
        }
        if (seed.fields.find("_deleted") == seed.fields.end()) {
            seed.fields["_deleted"] = mimicdb::FieldValue::Bool(false);
        }
        updates_out.push_back(std::move(seed));
    }
    if (updates_out.empty()) {
        return 0;
    }
    if (state && state->initialized) {
        for (const auto& doc : updates_out) {
            for (const auto& item : doc.fields) {
                if (state->field_index.find(item.first) == state->field_index.end()) {
                    if (error) {
                        *error = "unknown field for collection";
                    }
                    return matched;
                }
            }
        }
    }
    std::string append_error;
    InsertMany(db, collection, updates_out, &append_error);
    if (!append_error.empty() && error) {
        *error = append_error;
    }
    return matched;
}

size_t MongoClientCore::Delete(const std::string& db, const std::string& collection,
                               const std::vector<Filter>& filters,
                               bool multi, std::string* error) {
    auto* state = GetCollection(db, collection);
    if (!state) {
        if (error) {
            *error = "unknown collection";
        }
        return 0;
    }
    if (!UpdateCache(db, collection, {}, error)) {
        return 0;
    }
    IdFilterInfo id_filter;
    bool use_id_fast_path = ExtractIdFilter(filters, &id_filter);
    state->append_only = false;
    std::vector<MongoDocument> deletes;
    size_t matched = 0;
    for (const auto& item : state->latest_cache) {
        if (use_id_fast_path) {
            if (id_filter.is_eq && item.first != id_filter.eq_value) {
                continue;
            }
            if (id_filter.is_in &&
                std::find(id_filter.in_values.begin(), id_filter.in_values.end(),
                          item.first) == id_filter.in_values.end()) {
                continue;
            }
        } else if (!MatchFilters(item.second, filters)) {
            continue;
        }
        matched += 1;
        MongoDocument tombstone;
        tombstone.fields["_id"] = mimicdb::FieldValue::Int64(item.first);
        tombstone.fields["_deleted"] = mimicdb::FieldValue::Bool(true);
        tombstone.fields["_version"] =
            mimicdb::FieldValue::Int64(static_cast<int64_t>(NextVersion()));
        deletes.push_back(std::move(tombstone));
        if (!multi) {
            break;
        }
    }
    if (deletes.empty()) {
        return 0;
    }
    std::string append_error;
    InsertMany(db, collection, deletes, &append_error);
    if (!append_error.empty() && error) {
        *error = append_error;
    }
    return matched;
}

std::vector<MongoDocument> MongoClientCore::LatestDocuments(const std::string& db,
                                                            const std::string& collection,
                                                            std::string* error) const {
    return LatestDocuments(db, collection, {}, error);
}

std::vector<MongoDocument> MongoClientCore::LatestDocuments(const std::string& db,
                                                            const std::string& collection,
                                                            const std::vector<std::string>& required_fields,
                                                            std::string* error) const {
    std::vector<MongoDocument> docs;
    if (!UpdateCache(db, collection, required_fields, error)) {
        return docs;
    }
    const auto* state = GetCollection(db, collection);
    if (!state) {
        return docs;
    }
    docs.reserve(state->latest_cache.size());
    for (const auto& item : state->latest_cache) {
        docs.push_back(item.second);
    }
    return docs;
}

bool MongoClientCore::UpdateCache(const std::string& db,
                                  const std::string& collection,
                                  const std::vector<std::string>& required_fields,
                                  std::string* error) const {
    const auto* state = GetCollection(db, collection);
    if (!state) {
        if (error) {
            *error = "unknown collection";
        }
        return false;
    }

    std::unordered_set<std::string> required;
    std::vector<std::string> columns;
    if (required_fields.empty()) {
        for (const auto& field : state->fields) {
            AppendUnique(&columns, &required, field.name);
        }
    } else {
        AppendUnique(&columns, &required, "_id");
        AppendUnique(&columns, &required, "_version");
        AppendUnique(&columns, &required, "_deleted");
        for (const auto& name : required_fields) {
            AppendUnique(&columns, &required, name);
        }
    }

    bool full_rebuild = !state->cache_valid;
    if (!full_rebuild) {
        for (const auto& name : required) {
            if (state->cached_fields.find(name) == state->cached_fields.end()) {
                full_rebuild = true;
                break;
            }
        }
    }

    if (full_rebuild) {
        state->latest_cache.clear();
        state->cached_fields = required;
        state->last_seen_version = 0;
    }
    state->stats.last_full_rebuild = full_rebuild;

    std::vector<Predicate> predicates;
    if (!full_rebuild && state->last_seen_version > 0) {
        auto it = state->field_index.find("_version");
        if (it != state->field_index.end()) {
            Predicate pred;
            pred.field_index = it->second;
            pred.op = mimicdb::CompareOp::kGt;
            pred.value = static_cast<double>(state->last_seen_version);
            pred.value_type = mimicdb::FieldType::kInt64;
            predicates.push_back(std::move(pred));
        }
    }

    const std::vector<std::string> scan_columns = full_rebuild
        ? columns
        : std::vector<std::string>(state->cached_fields.begin(), state->cached_fields.end());

    ScanResult result = core_->Scan(db, collection, scan_columns, predicates, 0, 0, error);
    if (error && !error->empty()) {
        return false;
    }
    state->stats.last_scan_rows = result.rows.size();

    int64_t max_version = state->last_seen_version;
    for (const auto& row : result.rows) {
        MongoDocument doc;
        int64_t doc_id = -1;
        int64_t version = 0;
        bool deleted = false;
        for (size_t i = 0; i < result.columns.size(); ++i) {
            const auto& name = result.columns[i];
            doc.fields[name] = row[i];
            if (name == "_id" && !row[i].is_null) {
                doc_id = row[i].i64;
            } else if (name == "_version" && !row[i].is_null) {
                version = row[i].i64;
            } else if (name == "_deleted" && !row[i].is_null) {
                deleted = row[i].b;
            }
        }
        if (version > max_version) {
            max_version = version;
        }
        if (doc_id < 0 || deleted) {
            if (doc_id >= 0 && deleted) {
                state->latest_cache.erase(doc_id);
            }
            continue;
        }
        auto it = state->latest_cache.find(doc_id);
        if (it == state->latest_cache.end()) {
            state->latest_cache[doc_id] = std::move(doc);
            continue;
        }
        auto existing = it->second.fields.find("_version");
        if (existing == it->second.fields.end() || existing->second.i64 < version) {
            it->second = std::move(doc);
        } else if (!full_rebuild) {
            for (const auto& item : doc.fields) {
                it->second.fields[item.first] = item.second;
            }
        }
    }
    state->cache_valid = true;
    state->last_seen_version = max_version;
    state->stats.last_returned_rows = state->latest_cache.size();
    return true;
}

MongoStats MongoClientCore::StatsFor(const std::string& db,
                                     const std::string& collection) const {
    MongoStats stats;
    const auto* state = GetCollection(db, collection);
    if (!state) {
        return stats;
    }
    return state->stats;
}

}  // namespace mimicapi
