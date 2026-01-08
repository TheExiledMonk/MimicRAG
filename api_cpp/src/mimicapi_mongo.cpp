#include "mimicapi/mongo.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <functional>
#include <regex>

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
        return true;
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
    std::vector<mimicdb::FieldBatch> batches;
    std::vector<std::vector<int64_t>> i64_values;
    std::vector<std::vector<int32_t>> i32_values;
    std::vector<std::vector<double>> f64_values;
    std::vector<std::vector<uint8_t>> bool_values;
    std::vector<std::vector<uint32_t>> length_values;
    std::vector<std::vector<uint8_t>> bytes_values;
    std::vector<std::vector<uint8_t>> validity_buffers;

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
            i64_values.push_back(std::move(values));
            batch.data = i64_values.back().data();
        } else if (field.name == "_version") {
            std::vector<int64_t> values;
            values.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                values.push_back(static_cast<int64_t>(NextVersion()));
                validity.push_back(1);
            }
            i64_values.push_back(std::move(values));
            batch.data = i64_values.back().data();
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
            bool_values.push_back(std::move(values));
            batch.data = bool_values.back().data();
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
            i64_values.push_back(std::move(values));
            batch.data = i64_values.back().data();
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
            i32_values.push_back(std::move(values));
            batch.data = i32_values.back().data();
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
            f64_values.push_back(std::move(values));
            batch.data = f64_values.back().data();
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
            bool_values.push_back(std::move(values));
            batch.data = bool_values.back().data();
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
            length_values.push_back(std::move(lengths));
            bytes_values.push_back(std::move(bytes));
            batch.lengths = length_values.back().data();
            batch.bytes = bytes_values.back().data();
            batch.bytes_size = bytes_values.back().size();
        }
        validity_buffers.push_back(std::move(validity));
        batch.validity = validity_buffers.back().data();
        batches.push_back(batch);
    }
    return core_->AppendBatch(db, collection, batches, error);
}

std::vector<MongoDocument> MongoClientCore::Find(const std::string& db,
                                                 const std::string& collection,
                                                 const std::vector<Filter>& filters,
                                                 const ProjectionSpec& projection,
                                                 const FindOptions& options,
                                                 std::string* error) const {
    std::vector<MongoDocument> out;
    auto docs = LatestDocuments(db, collection, error);
    if (error && !error->empty()) {
        return out;
    }
    for (const auto& doc : docs) {
        if (!MatchFilters(doc, filters)) {
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
    auto docs = LatestDocuments(db, collection, error);
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

    out = execute_pipeline(std::move(docs), pipeline);
    return out;
}

size_t MongoClientCore::Update(const std::string& db, const std::string& collection,
                               const std::vector<Filter>& filters,
                               const UpdateSpec& update,
                               bool multi, bool upsert, bool replace,
                               std::string* error) {
    auto docs = LatestDocuments(db, collection, error);
    if (error && !error->empty()) {
        return 0;
    }
    auto* state = GetCollection(db, collection);
    std::vector<MongoDocument> updates_out;
    size_t matched = 0;
    for (auto& doc : docs) {
        if (!MatchFilters(doc, filters)) {
            continue;
        }
        matched += 1;
        MongoDocument updated;
        if (replace || update.is_replacement) {
            updated.fields = update.replacement;
            auto id_it = doc.fields.find("_id");
            if (id_it != doc.fields.end()) {
                updated.fields["_id"] = id_it->second;
            }
        } else {
            updated = doc;
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
    auto docs = LatestDocuments(db, collection, error);
    if (error && !error->empty()) {
        return 0;
    }
    std::vector<MongoDocument> deletes;
    size_t matched = 0;
    for (auto& doc : docs) {
        if (!MatchFilters(doc, filters)) {
            continue;
        }
        matched += 1;
        doc.fields["_deleted"] = mimicdb::FieldValue::Bool(true);
        doc.fields["_version"] = mimicdb::FieldValue::Int64(static_cast<int64_t>(NextVersion()));
        deletes.push_back(doc);
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
    std::vector<MongoDocument> docs;
    const auto* state = GetCollection(db, collection);
    if (!state) {
        if (error) {
            *error = "unknown collection";
        }
        return docs;
    }
    std::vector<std::string> columns;
    for (const auto& field : state->fields) {
        columns.push_back(field.name);
    }
    ScanResult result = core_->Scan(db, collection, columns, {}, 0, 0, error);
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
        if (it == latest.end() || it->second.fields["_version"].i64 < version) {
            latest[doc_id] = std::move(doc);
        }
    }
    for (auto& item : latest) {
        docs.push_back(std::move(item.second));
    }
    return docs;
}

}  // namespace mimicapi
