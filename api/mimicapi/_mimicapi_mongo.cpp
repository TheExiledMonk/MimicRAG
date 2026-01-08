#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <string>
#include <vector>

#include "mimicapi/mongo.h"

namespace {

struct MongoClientCoreObject {
    PyObject_HEAD
    mimicapi::ApiClientCore* api_core = nullptr;
    mimicapi::MongoClientCore* mongo_core = nullptr;
};

mimicdb::FieldValue ParseValue(PyObject* value) {
    if (value == Py_None) {
        return mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
    }
    if (PyBool_Check(value)) {
        return mimicdb::FieldValue::Bool(value == Py_True);
    }
    if (PyLong_Check(value)) {
        return mimicdb::FieldValue::Int64(PyLong_AsLongLong(value));
    }
    if (PyFloat_Check(value)) {
        return mimicdb::FieldValue::Float64(PyFloat_AsDouble(value));
    }
    if (PyUnicode_Check(value)) {
        PyObject* bytes = PyUnicode_AsUTF8String(value);
        if (!bytes) {
            return mimicdb::FieldValue::Null(mimicdb::FieldType::kString);
        }
        char* data = nullptr;
        Py_ssize_t size = 0;
        if (PyBytes_AsStringAndSize(bytes, &data, &size) != 0) {
            Py_DECREF(bytes);
            return mimicdb::FieldValue::Null(mimicdb::FieldType::kString);
        }
        std::string out(data, static_cast<size_t>(size));
        Py_DECREF(bytes);
        return mimicdb::FieldValue::String(out);
    }
    if (PyBytes_Check(value)) {
        char* data = nullptr;
        Py_ssize_t size = 0;
        if (PyBytes_AsStringAndSize(value, &data, &size) != 0) {
            return mimicdb::FieldValue::Null(mimicdb::FieldType::kBytes);
        }
        return mimicdb::FieldValue::Bytes(std::string(data, static_cast<size_t>(size)));
    }
    if (PyList_Check(value) || PyTuple_Check(value)) {
        PyObject* seq = PySequence_Fast(value, "array requires sequence");
        if (!seq) {
            return mimicdb::FieldValue::Null(mimicdb::FieldType::kArray);
        }
        const Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
        std::vector<mimicdb::FieldValue> items;
        items.reserve(static_cast<size_t>(len));
        for (Py_ssize_t i = 0; i < len; ++i) {
            PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
            items.push_back(ParseValue(item));
        }
        Py_DECREF(seq);
        return mimicdb::FieldValue::Array(items);
    }
    return mimicdb::FieldValue::Null(mimicdb::FieldType::kInt64);
}

PyObject* FieldValueToPy(const mimicdb::FieldValue& value) {
    if (value.is_null) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    switch (value.type) {
        case mimicdb::FieldType::kInt32:
            return PyLong_FromLong(value.i32);
        case mimicdb::FieldType::kInt64:
            return PyLong_FromLongLong(value.i64);
        case mimicdb::FieldType::kFloat64:
            return PyFloat_FromDouble(value.f64);
        case mimicdb::FieldType::kBool:
            return PyBool_FromLong(value.b ? 1 : 0);
        case mimicdb::FieldType::kString:
            return PyUnicode_FromStringAndSize(value.bytes.data(),
                                               static_cast<Py_ssize_t>(value.bytes.size()));
        case mimicdb::FieldType::kBytes:
            return PyBytes_FromStringAndSize(value.bytes.data(),
                                             static_cast<Py_ssize_t>(value.bytes.size()));
        case mimicdb::FieldType::kDictInt32:
            return PyLong_FromLong(value.i32);
        case mimicdb::FieldType::kArray: {
            PyObject* list = PyList_New(static_cast<Py_ssize_t>(value.array.size()));
            for (size_t i = 0; i < value.array.size(); ++i) {
                PyObject* item = FieldValueToPy(value.array[i]);
                if (!item) {
                    Py_DECREF(list);
                    return nullptr;
                }
                PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item);
            }
            return list;
        }
        case mimicdb::FieldType::kObject: {
            PyObject* dict = PyDict_New();
            for (const auto& item : value.object) {
                PyObject* py_value = FieldValueToPy(item.second);
                if (!py_value) {
                    Py_DECREF(dict);
                    return nullptr;
                }
                PyDict_SetItemString(dict, item.first.c_str(), py_value);
                Py_DECREF(py_value);
            }
            return dict;
        }
    }
    Py_INCREF(Py_None);
    return Py_None;
}

mimicapi::FilterOp ParseFilterOp(const std::string& op, bool* ok) {
    *ok = true;
    if (op == "$eq") {
        return mimicapi::FilterOp::kEq;
    }
    if (op == "$ne") {
        return mimicapi::FilterOp::kNe;
    }
    if (op == "$gt") {
        return mimicapi::FilterOp::kGt;
    }
    if (op == "$lt") {
        return mimicapi::FilterOp::kLt;
    }
    if (op == "$in") {
        return mimicapi::FilterOp::kIn;
    }
    if (op == "$nin") {
        return mimicapi::FilterOp::kNin;
    }
    if (op == "$all") {
        return mimicapi::FilterOp::kAll;
    }
    if (op == "$size") {
        return mimicapi::FilterOp::kSize;
    }
    if (op == "$regex") {
        return mimicapi::FilterOp::kRegex;
    }
    if (op == "$exists") {
        return mimicapi::FilterOp::kExists;
    }
    *ok = false;
    return mimicapi::FilterOp::kEq;
}

bool ParseFilters(PyObject* filter_obj, std::vector<mimicapi::Filter>* out) {
    if (!filter_obj || filter_obj == Py_None) {
        return true;
    }
    if (!PyDict_Check(filter_obj)) {
        PyErr_SetString(PyExc_TypeError, "filter must be dict");
        return false;
    }
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(filter_obj, &pos, &key, &value)) {
        if (!PyUnicode_Check(key)) {
            PyErr_SetString(PyExc_TypeError, "filter field must be string");
            return false;
        }
        PyObject* key_bytes = PyUnicode_AsUTF8String(key);
        if (!key_bytes) {
            return false;
        }
        std::string field = PyBytes_AsString(key_bytes);
        Py_DECREF(key_bytes);
        if (PyDict_Check(value)) {
            PyObject* op_key = nullptr;
            PyObject* op_val = nullptr;
            Py_ssize_t op_pos = 0;
            while (PyDict_Next(value, &op_pos, &op_key, &op_val)) {
                if (!PyUnicode_Check(op_key)) {
                    PyErr_SetString(PyExc_TypeError, "operator must be string");
                    return false;
                }
                PyObject* op_bytes = PyUnicode_AsUTF8String(op_key);
                if (!op_bytes) {
                    return false;
                }
                std::string op = PyBytes_AsString(op_bytes);
                Py_DECREF(op_bytes);
                if (op == "$not") {
                    if (!PyDict_Check(op_val)) {
                        PyErr_SetString(PyExc_TypeError, "$not must be dict");
                        return false;
                    }
                    std::vector<mimicapi::Filter> nested;
                    if (!ParseFilters(op_val, &nested)) {
                        return false;
                    }
                    for (auto& child : nested) {
                        if (child.field.empty()) {
                            continue;
                        }
                        child.negated = true;
                        out->push_back(std::move(child));
                    }
                    continue;
                }
                bool ok = false;
                const auto op_enum = ParseFilterOp(op, &ok);
                if (!ok) {
                    PyErr_SetString(PyExc_ValueError, "unsupported operator");
                    return false;
                }
                mimicapi::Filter filter;
                filter.field = field;
                filter.op = op_enum;
                if (op_enum == mimicapi::FilterOp::kExists) {
                    filter.exists = PyObject_IsTrue(op_val) != 0;
                } else if (op_enum == mimicapi::FilterOp::kRegex) {
                    if (!PyUnicode_Check(op_val)) {
                        PyErr_SetString(PyExc_TypeError, "$regex must be string");
                        return false;
                    }
                    PyObject* regex_bytes = PyUnicode_AsUTF8String(op_val);
                    if (!regex_bytes) {
                        return false;
                    }
                    filter.regex = PyBytes_AsString(regex_bytes);
                    Py_DECREF(regex_bytes);
                    PyObject* options_obj = PyDict_GetItemString(value, "$options");
                    if (options_obj && PyUnicode_Check(options_obj)) {
                        PyObject* opt_bytes = PyUnicode_AsUTF8String(options_obj);
                        if (!opt_bytes) {
                            return false;
                        }
                        filter.regex_options = PyBytes_AsString(opt_bytes);
                        Py_DECREF(opt_bytes);
                    }
                } else if (op_enum == mimicapi::FilterOp::kIn) {
                    PyObject* seq = PySequence_Fast(op_val, "in requires sequence");
                    if (!seq) {
                        return false;
                    }
                    const Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
                    for (Py_ssize_t i = 0; i < len; ++i) {
                        PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
                        filter.values.push_back(ParseValue(item));
                    }
                    Py_DECREF(seq);
                } else if (op_enum == mimicapi::FilterOp::kNin) {
                    PyObject* seq = PySequence_Fast(op_val, "nin requires sequence");
                    if (!seq) {
                        return false;
                    }
                    const Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
                    for (Py_ssize_t i = 0; i < len; ++i) {
                        PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
                        filter.values.push_back(ParseValue(item));
                    }
                    Py_DECREF(seq);
                } else if (op_enum == mimicapi::FilterOp::kAll) {
                    PyObject* seq = PySequence_Fast(op_val, "all requires sequence");
                    if (!seq) {
                        return false;
                    }
                    const Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
                    for (Py_ssize_t i = 0; i < len; ++i) {
                        PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
                        filter.values.push_back(ParseValue(item));
                    }
                    Py_DECREF(seq);
                } else if (op_enum == mimicapi::FilterOp::kSize) {
                    if (!PyLong_Check(op_val)) {
                        PyErr_SetString(PyExc_TypeError, "$size must be int");
                        return false;
                    }
                    filter.values.push_back(ParseValue(op_val));
                } else {
                    filter.values.push_back(ParseValue(op_val));
                }
                out->push_back(std::move(filter));
            }
        } else if (PyList_Check(value)) {
            mimicapi::Filter filter;
            filter.field = field;
            filter.op = mimicapi::FilterOp::kEq;
            filter.values.push_back(ParseValue(value));
            out->push_back(std::move(filter));
        } else {
            mimicapi::Filter filter;
            filter.field = field;
            filter.op = mimicapi::FilterOp::kEq;
            filter.values.push_back(ParseValue(value));
            out->push_back(std::move(filter));
        }
    }
    return true;
}

bool ParseFieldFilters(PyObject* filter_obj, std::vector<mimicapi::Filter>* out) {
    if (!filter_obj || filter_obj == Py_None) {
        return true;
    }
    if (!PyDict_Check(filter_obj)) {
        PyErr_SetString(PyExc_TypeError, "filter must be dict");
        return false;
    }
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(filter_obj, &pos, &key, &value)) {
        if (!PyUnicode_Check(key)) {
            PyErr_SetString(PyExc_TypeError, "filter field must be string");
            return false;
        }
        PyObject* key_bytes = PyUnicode_AsUTF8String(key);
        if (!key_bytes) {
            return false;
        }
        std::string field = PyBytes_AsString(key_bytes);
        Py_DECREF(key_bytes);
        if (!field.empty() && field[0] == '$') {
            continue;
        }
        if (PyDict_Check(value)) {
            PyObject* op_key = nullptr;
            PyObject* op_val = nullptr;
            Py_ssize_t op_pos = 0;
            while (PyDict_Next(value, &op_pos, &op_key, &op_val)) {
                if (!PyUnicode_Check(op_key)) {
                    PyErr_SetString(PyExc_TypeError, "operator must be string");
                    return false;
                }
                PyObject* op_bytes = PyUnicode_AsUTF8String(op_key);
                if (!op_bytes) {
                    return false;
                }
                std::string op = PyBytes_AsString(op_bytes);
                Py_DECREF(op_bytes);
                if (op == "$not") {
                    if (!PyDict_Check(op_val)) {
                        PyErr_SetString(PyExc_TypeError, "$not must be dict");
                        return false;
                    }
                    std::vector<mimicapi::Filter> nested;
                    if (!ParseFieldFilters(op_val, &nested)) {
                        return false;
                    }
                    for (auto& child : nested) {
                        if (child.field.empty()) {
                            continue;
                        }
                        child.negated = true;
                        out->push_back(std::move(child));
                    }
                    continue;
                }
                bool ok = false;
                const auto op_enum = ParseFilterOp(op, &ok);
                if (!ok) {
                    PyErr_SetString(PyExc_ValueError, "unsupported operator");
                    return false;
                }
                mimicapi::Filter filter;
                filter.field = field;
                filter.op = op_enum;
                if (op_enum == mimicapi::FilterOp::kExists) {
                    filter.exists = PyObject_IsTrue(op_val) != 0;
                } else if (op_enum == mimicapi::FilterOp::kRegex) {
                    if (!PyUnicode_Check(op_val)) {
                        PyErr_SetString(PyExc_TypeError, "$regex must be string");
                        return false;
                    }
                    PyObject* regex_bytes = PyUnicode_AsUTF8String(op_val);
                    if (!regex_bytes) {
                        return false;
                    }
                    filter.regex = PyBytes_AsString(regex_bytes);
                    Py_DECREF(regex_bytes);
                    PyObject* options_obj = PyDict_GetItemString(value, "$options");
                    if (options_obj && PyUnicode_Check(options_obj)) {
                        PyObject* opt_bytes = PyUnicode_AsUTF8String(options_obj);
                        if (!opt_bytes) {
                            return false;
                        }
                        filter.regex_options = PyBytes_AsString(opt_bytes);
                        Py_DECREF(opt_bytes);
                    }
                } else if (op_enum == mimicapi::FilterOp::kIn ||
                           op_enum == mimicapi::FilterOp::kNin ||
                           op_enum == mimicapi::FilterOp::kAll) {
                    PyObject* seq = PySequence_Fast(op_val, "operator requires sequence");
                    if (!seq) {
                        return false;
                    }
                    const Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
                    for (Py_ssize_t i = 0; i < len; ++i) {
                        PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
                        filter.values.push_back(ParseValue(item));
                    }
                    Py_DECREF(seq);
                } else if (op_enum == mimicapi::FilterOp::kSize) {
                    if (!PyLong_Check(op_val)) {
                        PyErr_SetString(PyExc_TypeError, "$size must be int");
                        return false;
                    }
                    filter.values.push_back(ParseValue(op_val));
                } else {
                    filter.values.push_back(ParseValue(op_val));
                }
                out->push_back(std::move(filter));
            }
        } else {
            mimicapi::Filter filter;
            filter.field = field;
            filter.op = mimicapi::FilterOp::kEq;
            filter.values.push_back(ParseValue(value));
            out->push_back(std::move(filter));
        }
    }
    return true;
}

bool ParseMatchExpression(PyObject* match_obj, mimicapi::MatchExpression* out) {
    if (!out) {
        return false;
    }
    *out = mimicapi::MatchExpression();
    if (!match_obj || match_obj == Py_None) {
        return true;
    }
    if (!PyDict_Check(match_obj)) {
        PyErr_SetString(PyExc_TypeError, "$match must be dict");
        return false;
    }
    if (!ParseFieldFilters(match_obj, &out->filters)) {
        return false;
    }
    PyObject* and_obj = PyDict_GetItemString(match_obj, "$and");
    if (and_obj) {
        PyObject* seq = PySequence_Fast(and_obj, "$and requires list");
        if (!seq) {
            return false;
        }
        const Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
        for (Py_ssize_t i = 0; i < len; ++i) {
            PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
            mimicapi::MatchExpression child;
            if (!ParseMatchExpression(item, &child)) {
                Py_DECREF(seq);
                return false;
            }
            out->children.push_back(std::move(child));
        }
        Py_DECREF(seq);
    }
    PyObject* or_obj = PyDict_GetItemString(match_obj, "$or");
    if (or_obj) {
        PyObject* seq = PySequence_Fast(or_obj, "$or requires list");
        if (!seq) {
            return false;
        }
        mimicapi::MatchExpression clause;
        clause.op = mimicapi::MatchOp::kOr;
        const Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
        for (Py_ssize_t i = 0; i < len; ++i) {
            PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
            mimicapi::MatchExpression child;
            if (!ParseMatchExpression(item, &child)) {
                Py_DECREF(seq);
                return false;
            }
            clause.children.push_back(std::move(child));
        }
        Py_DECREF(seq);
        out->children.push_back(std::move(clause));
    }
    PyObject* nor_obj = PyDict_GetItemString(match_obj, "$nor");
    if (nor_obj) {
        PyObject* seq = PySequence_Fast(nor_obj, "$nor requires list");
        if (!seq) {
            return false;
        }
        mimicapi::MatchExpression clause;
        clause.op = mimicapi::MatchOp::kNor;
        const Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
        for (Py_ssize_t i = 0; i < len; ++i) {
            PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
            mimicapi::MatchExpression child;
            if (!ParseMatchExpression(item, &child)) {
                Py_DECREF(seq);
                return false;
            }
            clause.children.push_back(std::move(child));
        }
        Py_DECREF(seq);
        out->children.push_back(std::move(clause));
    }
    return true;
}

int MongoClientCoreInit(MongoClientCoreObject* self, PyObject*, PyObject*) {
    self->api_core = new mimicapi::ApiClientCore();
    self->mongo_core = new mimicapi::MongoClientCore(self->api_core);
    return 0;
}

void MongoClientCoreDealloc(MongoClientCoreObject* self) {
    delete self->mongo_core;
    delete self->api_core;
    self->mongo_core = nullptr;
    self->api_core = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

PyObject* MongoClientInsertMany(MongoClientCoreObject* self, PyObject* args) {
    const char* db = nullptr;
    const char* collection = nullptr;
    PyObject* docs_obj = nullptr;
    if (!PyArg_ParseTuple(args, "ssO", &db, &collection, &docs_obj)) {
        return nullptr;
    }
    if (!PyList_Check(docs_obj)) {
        PyErr_SetString(PyExc_TypeError, "docs must be a list");
        return nullptr;
    }
    std::vector<mimicapi::MongoDocument> docs;
    const Py_ssize_t count = PyList_Size(docs_obj);
    docs.reserve(static_cast<size_t>(count));
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject* item = PyList_GetItem(docs_obj, i);
        if (!PyDict_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "doc must be dict");
            return nullptr;
        }
        mimicapi::MongoDocument doc;
        PyObject* key = nullptr;
        PyObject* value = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(item, &pos, &key, &value)) {
            if (!PyUnicode_Check(key)) {
                PyErr_SetString(PyExc_TypeError, "field name must be string");
                return nullptr;
            }
            PyObject* key_bytes = PyUnicode_AsUTF8String(key);
            if (!key_bytes) {
                return nullptr;
            }
            std::string field = PyBytes_AsString(key_bytes);
            Py_DECREF(key_bytes);
            doc.fields[field] = ParseValue(value);
        }
        docs.push_back(std::move(doc));
    }
    std::string error;
    if (!self->mongo_core->InsertMany(db, collection, docs, &error)) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

bool ParseSort(PyObject* sort_obj, std::vector<mimicapi::SortSpec>* out) {
    if (!sort_obj || sort_obj == Py_None) {
        return true;
    }
    if (PyDict_Check(sort_obj)) {
        PyObject* key = nullptr;
        PyObject* value = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(sort_obj, &pos, &key, &value)) {
            if (!PyUnicode_Check(key) || !PyLong_Check(value)) {
                PyErr_SetString(PyExc_TypeError, "sort dict requires str keys and int values");
                return false;
            }
            PyObject* key_bytes = PyUnicode_AsUTF8String(key);
            if (!key_bytes) {
                return false;
            }
            mimicapi::SortSpec spec;
            spec.field = PyBytes_AsString(key_bytes);
            Py_DECREF(key_bytes);
            spec.direction = PyLong_AsLong(value) < 0 ? -1 : 1;
            out->push_back(std::move(spec));
        }
        return true;
    }
    if (PyList_Check(sort_obj) || PyTuple_Check(sort_obj)) {
        PyObject* seq = PySequence_Fast(sort_obj, "sort requires sequence");
        if (!seq) {
            return false;
        }
        const Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
        for (Py_ssize_t i = 0; i < len; ++i) {
            PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
            if (!PyTuple_Check(item) || PyTuple_Size(item) != 2) {
                Py_DECREF(seq);
                PyErr_SetString(PyExc_TypeError, "sort entries must be (field, direction)");
                return false;
            }
            PyObject* field_obj = PyTuple_GetItem(item, 0);
            PyObject* dir_obj = PyTuple_GetItem(item, 1);
            if (!PyUnicode_Check(field_obj) || !PyLong_Check(dir_obj)) {
                Py_DECREF(seq);
                PyErr_SetString(PyExc_TypeError, "sort entries require str, int");
                return false;
            }
            PyObject* field_bytes = PyUnicode_AsUTF8String(field_obj);
            if (!field_bytes) {
                Py_DECREF(seq);
                return false;
            }
            mimicapi::SortSpec spec;
            spec.field = PyBytes_AsString(field_bytes);
            Py_DECREF(field_bytes);
            spec.direction = PyLong_AsLong(dir_obj) < 0 ? -1 : 1;
            out->push_back(std::move(spec));
        }
        Py_DECREF(seq);
        return true;
    }
    PyErr_SetString(PyExc_TypeError, "sort must be dict or list");
    return false;
}

bool ParsePipelineStages(PyObject* pipeline_obj,
                         std::vector<mimicapi::PipelineStage>* out) {
    if (!pipeline_obj || pipeline_obj == Py_None) {
        return true;
    }
    if (!PyList_Check(pipeline_obj)) {
        PyErr_SetString(PyExc_TypeError, "pipeline must be list");
        return false;
    }
    const Py_ssize_t stages = PyList_Size(pipeline_obj);
    for (Py_ssize_t i = 0; i < stages; ++i) {
        PyObject* stage = PyList_GetItem(pipeline_obj, i);
        if (!PyDict_Check(stage)) {
            PyErr_SetString(PyExc_TypeError, "stage must be dict");
            return false;
        }
        PyObject* match_obj = PyDict_GetItemString(stage, "$match");
        if (match_obj) {
            mimicapi::PipelineStage out_stage;
            out_stage.type = mimicapi::StageType::kMatch;
            if (!ParseMatchExpression(match_obj, &out_stage.match)) {
                return false;
            }
            out->push_back(std::move(out_stage));
        }
        PyObject* group_obj = PyDict_GetItemString(stage, "$group");
        if (group_obj) {
            if (!PyDict_Check(group_obj)) {
                PyErr_SetString(PyExc_TypeError, "$group must be dict");
                return false;
            }
            mimicapi::PipelineStage out_stage;
            out_stage.type = mimicapi::StageType::kGroup;
            PyObject* id_obj = PyDict_GetItemString(group_obj, "_id");
            if (id_obj == Py_None || id_obj == nullptr) {
                out_stage.group.has_id = false;
            } else if (PyUnicode_Check(id_obj)) {
                PyObject* id_bytes = PyUnicode_AsUTF8String(id_obj);
                if (!id_bytes) {
                    return false;
                }
                std::string id = PyBytes_AsString(id_bytes);
                Py_DECREF(id_bytes);
                if (!id.empty() && id[0] == '$') {
                    id = id.substr(1);
                }
                out_stage.group.has_id = true;
                out_stage.group.field = id;
            } else if (PyDict_Check(id_obj)) {
                PyObject* gid_key = nullptr;
                PyObject* gid_val = nullptr;
                Py_ssize_t gid_pos = 0;
                while (PyDict_Next(id_obj, &gid_pos, &gid_key, &gid_val)) {
                    if (!PyUnicode_Check(gid_key) || !PyUnicode_Check(gid_val)) {
                        PyErr_SetString(PyExc_TypeError, "group _id dict must map to field refs");
                        return false;
                    }
                    PyObject* key_bytes = PyUnicode_AsUTF8String(gid_key);
                    PyObject* val_bytes = PyUnicode_AsUTF8String(gid_val);
                    if (!key_bytes || !val_bytes) {
                        Py_XDECREF(key_bytes);
                        Py_XDECREF(val_bytes);
                        return false;
                    }
                    std::string name = PyBytes_AsString(key_bytes);
                    std::string field = PyBytes_AsString(val_bytes);
                    Py_DECREF(key_bytes);
                    Py_DECREF(val_bytes);
                    if (!field.empty() && field[0] == '$') {
                        field = field.substr(1);
                    }
                    out_stage.group.fields.push_back({name, field});
                }
                if (out_stage.group.fields.empty()) {
                    PyErr_SetString(PyExc_TypeError, "group _id dict must not be empty");
                    return false;
                }
                out_stage.group.has_id = true;
            } else {
                PyErr_SetString(PyExc_TypeError, "group _id must be string, dict, or None");
                return false;
            }
            PyObject* key = nullptr;
            PyObject* value = nullptr;
            Py_ssize_t pos = 0;
            while (PyDict_Next(group_obj, &pos, &key, &value)) {
                if (!PyUnicode_Check(key)) {
                    PyErr_SetString(PyExc_TypeError, "group field name must be string");
                    return false;
                }
                PyObject* key_bytes = PyUnicode_AsUTF8String(key);
                if (!key_bytes) {
                    return false;
                }
                std::string name = PyBytes_AsString(key_bytes);
                Py_DECREF(key_bytes);
                if (name == "_id") {
                    continue;
                }
                if (!PyDict_Check(value)) {
                    PyErr_SetString(PyExc_TypeError, "group op must be dict");
                    return false;
                }
                if (PyDict_Size(value) != 1) {
                    PyErr_SetString(PyExc_ValueError, "group op must have single key");
                    return false;
                }
                PyObject* op_key = nullptr;
                PyObject* op_val = nullptr;
                Py_ssize_t op_pos = 0;
                PyDict_Next(value, &op_pos, &op_key, &op_val);
                PyObject* op_bytes = PyUnicode_AsUTF8String(op_key);
                if (!op_bytes) {
                    return false;
                }
                std::string op = PyBytes_AsString(op_bytes);
                Py_DECREF(op_bytes);
                mimicapi::AggregateOp agg;
                agg.name = name;
                agg.op = op;
                if (op == "$sum" && PyLong_Check(op_val) && PyLong_AsLongLong(op_val) == 1) {
                    agg.count_only = true;
                } else if (PyUnicode_Check(op_val)) {
                    PyObject* field_bytes = PyUnicode_AsUTF8String(op_val);
                    if (!field_bytes) {
                        return false;
                    }
                    std::string field = PyBytes_AsString(field_bytes);
                    Py_DECREF(field_bytes);
                    if (!field.empty() && field[0] == '$') {
                        field = field.substr(1);
                    }
                    agg.field = field;
                } else {
                    PyErr_SetString(PyExc_TypeError, "group op value must be field reference");
                    return false;
                }
                out_stage.ops.push_back(std::move(agg));
            }
            out->push_back(std::move(out_stage));
        }
        PyObject* count_obj = PyDict_GetItemString(stage, "$count");
        if (count_obj) {
            if (!PyUnicode_Check(count_obj)) {
                PyErr_SetString(PyExc_TypeError, "$count must be string field name");
                return false;
            }
            PyObject* count_bytes = PyUnicode_AsUTF8String(count_obj);
            if (!count_bytes) {
                return false;
            }
            mimicapi::PipelineStage out_stage;
            out_stage.type = mimicapi::StageType::kCount;
            out_stage.count_field = PyBytes_AsString(count_bytes);
            Py_DECREF(count_bytes);
            out->push_back(std::move(out_stage));
        }
        PyObject* sort_by_count_obj = PyDict_GetItemString(stage, "$sortByCount");
        if (sort_by_count_obj) {
            mimicapi::PipelineStage out_stage;
            out_stage.type = mimicapi::StageType::kSortByCount;
            if (PyUnicode_Check(sort_by_count_obj)) {
                PyObject* field_bytes = PyUnicode_AsUTF8String(sort_by_count_obj);
                if (!field_bytes) {
                    return false;
                }
                std::string field = PyBytes_AsString(field_bytes);
                Py_DECREF(field_bytes);
                if (!field.empty() && field[0] == '$') {
                    out_stage.sort_by_count.is_field = true;
                    out_stage.sort_by_count.field = field.substr(1);
                } else {
                    out_stage.sort_by_count.is_field = false;
                    out_stage.sort_by_count.literal = mimicdb::FieldValue::String(field);
                }
            } else {
                out_stage.sort_by_count.is_field = false;
                out_stage.sort_by_count.literal = ParseValue(sort_by_count_obj);
            }
            out->push_back(std::move(out_stage));
        }
        PyObject* add_fields_obj = PyDict_GetItemString(stage, "$addFields");
        if (!add_fields_obj) {
            add_fields_obj = PyDict_GetItemString(stage, "$set");
        }
        if (add_fields_obj) {
            if (!PyDict_Check(add_fields_obj)) {
                PyErr_SetString(PyExc_TypeError, "$addFields must be dict");
                return false;
            }
            mimicapi::PipelineStage out_stage;
            out_stage.type = mimicapi::StageType::kAddFields;
            PyObject* key = nullptr;
            PyObject* value = nullptr;
            Py_ssize_t pos = 0;
            while (PyDict_Next(add_fields_obj, &pos, &key, &value)) {
                if (!PyUnicode_Check(key)) {
                    PyErr_SetString(PyExc_TypeError, "addFields key must be string");
                    return false;
                }
                PyObject* key_bytes = PyUnicode_AsUTF8String(key);
                if (!key_bytes) {
                    return false;
                }
                std::string name = PyBytes_AsString(key_bytes);
                Py_DECREF(key_bytes);
                mimicapi::ComputedField computed;
                computed.name = name;
                if (PyDict_Check(value)) {
                    PyObject* literal = PyDict_GetItemString(value, "$literal");
                    PyObject* field = PyDict_GetItemString(value, "$field");
                    if (literal) {
                        computed.from_field = false;
                        computed.literal = ParseValue(literal);
                    } else if (field && PyUnicode_Check(field)) {
                        PyObject* field_bytes = PyUnicode_AsUTF8String(field);
                        if (!field_bytes) {
                            return false;
                        }
                        computed.from_field = true;
                        computed.field = PyBytes_AsString(field_bytes);
                        Py_DECREF(field_bytes);
                    } else {
                        PyErr_SetString(PyExc_TypeError, "unsupported addFields expression");
                        return false;
                    }
                } else if (PyUnicode_Check(value)) {
                    PyObject* field_bytes = PyUnicode_AsUTF8String(value);
                    if (!field_bytes) {
                        return false;
                    }
                    std::string field = PyBytes_AsString(field_bytes);
                    Py_DECREF(field_bytes);
                    if (!field.empty() && field[0] == '$') {
                        computed.from_field = true;
                        computed.field = field.substr(1);
                    } else {
                        computed.from_field = false;
                        computed.literal = ParseValue(value);
                    }
                } else {
                    computed.from_field = false;
                    computed.literal = ParseValue(value);
                }
                out_stage.add_fields.push_back(std::move(computed));
            }
            out->push_back(std::move(out_stage));
        }
        PyObject* project_obj = PyDict_GetItemString(stage, "$project");
        if (project_obj) {
            if (!PyDict_Check(project_obj)) {
                PyErr_SetString(PyExc_TypeError, "$project must be dict");
                return false;
            }
            mimicapi::PipelineStage out_stage;
            out_stage.type = mimicapi::StageType::kProject;
            PyObject* key = nullptr;
            PyObject* value = nullptr;
            Py_ssize_t pos = 0;
            while (PyDict_Next(project_obj, &pos, &key, &value)) {
                if (!PyUnicode_Check(key)) {
                    PyErr_SetString(PyExc_TypeError, "project key must be string");
                    return false;
                }
                PyObject* key_bytes = PyUnicode_AsUTF8String(key);
                if (!key_bytes) {
                    return false;
                }
                std::string name = PyBytes_AsString(key_bytes);
                Py_DECREF(key_bytes);
                if (PyBool_Check(value) || PyLong_Check(value)) {
                    const long flag = PyLong_AsLong(value);
                    if (flag) {
                        out_stage.project.include.push_back(name);
                    } else {
                        out_stage.project.exclude.push_back(name);
                    }
                } else if (PyDict_Check(value)) {
                    PyObject* literal = PyDict_GetItemString(value, "$literal");
                    PyObject* field = PyDict_GetItemString(value, "$field");
                    mimicapi::ComputedField computed;
                    computed.name = name;
                    if (literal) {
                        computed.from_field = false;
                        computed.literal = ParseValue(literal);
                    } else if (field && PyUnicode_Check(field)) {
                        PyObject* field_bytes = PyUnicode_AsUTF8String(field);
                        if (!field_bytes) {
                            return false;
                        }
                        computed.from_field = true;
                        computed.field = PyBytes_AsString(field_bytes);
                        Py_DECREF(field_bytes);
                    } else {
                        PyErr_SetString(PyExc_TypeError, "unsupported project expression");
                        return false;
                    }
                    out_stage.project.computed.push_back(std::move(computed));
                } else if (PyUnicode_Check(value)) {
                    PyObject* field_bytes = PyUnicode_AsUTF8String(value);
                    if (!field_bytes) {
                        return false;
                    }
                    std::string field = PyBytes_AsString(field_bytes);
                    Py_DECREF(field_bytes);
                    if (!field.empty() && field[0] == '$') {
                        mimicapi::ComputedField computed;
                        computed.name = name;
                        computed.from_field = true;
                        computed.field = field.substr(1);
                        out_stage.project.computed.push_back(std::move(computed));
                    } else {
                        PyErr_SetString(PyExc_TypeError, "unsupported project expression");
                        return false;
                    }
                } else {
                    PyErr_SetString(PyExc_TypeError, "unsupported project expression");
                    return false;
                }
            }
            out->push_back(std::move(out_stage));
        }
        PyObject* unwind_obj = PyDict_GetItemString(stage, "$unwind");
        if (unwind_obj) {
            mimicapi::PipelineStage out_stage;
            out_stage.type = mimicapi::StageType::kUnwind;
            if (PyUnicode_Check(unwind_obj)) {
                PyObject* path_bytes = PyUnicode_AsUTF8String(unwind_obj);
                if (!path_bytes) {
                    return false;
                }
                std::string path = PyBytes_AsString(path_bytes);
                Py_DECREF(path_bytes);
                if (!path.empty() && path[0] == '$') {
                    path = path.substr(1);
                }
                out_stage.unwind.field = path;
                out_stage.unwind.preserve_null = false;
            } else if (PyDict_Check(unwind_obj)) {
                PyObject* path_obj = PyDict_GetItemString(unwind_obj, "path");
                if (!path_obj || !PyUnicode_Check(path_obj)) {
                    PyErr_SetString(PyExc_TypeError, "$unwind path must be string");
                    return false;
                }
                PyObject* path_bytes = PyUnicode_AsUTF8String(path_obj);
                if (!path_bytes) {
                    return false;
                }
                std::string path = PyBytes_AsString(path_bytes);
                Py_DECREF(path_bytes);
                if (!path.empty() && path[0] == '$') {
                    path = path.substr(1);
                }
                out_stage.unwind.field = path;
                PyObject* preserve = PyDict_GetItemString(unwind_obj, "preserveNullAndEmptyArrays");
                out_stage.unwind.preserve_null = preserve && PyObject_IsTrue(preserve) != 0;
            } else {
                PyErr_SetString(PyExc_TypeError, "$unwind must be string or dict");
                return false;
            }
            out->push_back(std::move(out_stage));
        }
        PyObject* lookup_obj = PyDict_GetItemString(stage, "$lookup");
        if (lookup_obj) {
            if (!PyDict_Check(lookup_obj)) {
                PyErr_SetString(PyExc_TypeError, "$lookup must be dict");
                return false;
            }
            PyObject* from_obj = PyDict_GetItemString(lookup_obj, "from");
            PyObject* local_obj = PyDict_GetItemString(lookup_obj, "localField");
            PyObject* foreign_obj = PyDict_GetItemString(lookup_obj, "foreignField");
            PyObject* as_obj = PyDict_GetItemString(lookup_obj, "as");
            if (!from_obj || !local_obj || !foreign_obj || !as_obj ||
                !PyUnicode_Check(from_obj) || !PyUnicode_Check(local_obj) ||
                !PyUnicode_Check(foreign_obj) || !PyUnicode_Check(as_obj)) {
                PyErr_SetString(PyExc_TypeError, "$lookup requires from/localField/foreignField/as strings");
                return false;
            }
            mimicapi::PipelineStage out_stage;
            out_stage.type = mimicapi::StageType::kLookup;
            PyObject* from_bytes = PyUnicode_AsUTF8String(from_obj);
            PyObject* local_bytes = PyUnicode_AsUTF8String(local_obj);
            PyObject* foreign_bytes = PyUnicode_AsUTF8String(foreign_obj);
            PyObject* as_bytes = PyUnicode_AsUTF8String(as_obj);
            if (!from_bytes || !local_bytes || !foreign_bytes || !as_bytes) {
                Py_XDECREF(from_bytes);
                Py_XDECREF(local_bytes);
                Py_XDECREF(foreign_bytes);
                Py_XDECREF(as_bytes);
                return false;
            }
            out_stage.lookup.from = PyBytes_AsString(from_bytes);
            out_stage.lookup.local_field = PyBytes_AsString(local_bytes);
            out_stage.lookup.foreign_field = PyBytes_AsString(foreign_bytes);
            out_stage.lookup.as_field = PyBytes_AsString(as_bytes);
            Py_DECREF(from_bytes);
            Py_DECREF(local_bytes);
            Py_DECREF(foreign_bytes);
            Py_DECREF(as_bytes);
            out->push_back(std::move(out_stage));
        }
        PyObject* facet_obj = PyDict_GetItemString(stage, "$facet");
        if (facet_obj) {
            if (!PyDict_Check(facet_obj)) {
                PyErr_SetString(PyExc_TypeError, "$facet must be dict");
                return false;
            }
            mimicapi::PipelineStage out_stage;
            out_stage.type = mimicapi::StageType::kFacet;
            PyObject* key = nullptr;
            PyObject* value = nullptr;
            Py_ssize_t pos = 0;
            while (PyDict_Next(facet_obj, &pos, &key, &value)) {
                if (!PyUnicode_Check(key)) {
                    PyErr_SetString(PyExc_TypeError, "$facet keys must be strings");
                    return false;
                }
                PyObject* key_bytes = PyUnicode_AsUTF8String(key);
                if (!key_bytes) {
                    return false;
                }
                std::string name = PyBytes_AsString(key_bytes);
                Py_DECREF(key_bytes);
                if (!PyList_Check(value)) {
                    PyErr_SetString(PyExc_TypeError, "$facet values must be pipeline lists");
                    return false;
                }
                std::vector<mimicapi::PipelineStage> branch;
                if (!ParsePipelineStages(value, &branch)) {
                    return false;
                }
                out_stage.facet.branches.push_back({name, std::move(branch)});
            }
            out->push_back(std::move(out_stage));
        }
    }
    return true;
}

PyObject* MongoClientFind(MongoClientCoreObject* self, PyObject* args, PyObject* kwargs) {
    const char* db = nullptr;
    const char* collection = nullptr;
    PyObject* filter_obj = Py_None;
    PyObject* projection_obj = Py_None;
    PyObject* sort_obj = Py_None;
    long skip = 0;
    long limit = 0;
    static const char* kwlist[] = {"db", "collection", "filter", "projection",
                                   "sort", "skip", "limit", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ss|OOOll",
                                     const_cast<char**>(kwlist),
                                     &db, &collection, &filter_obj, &projection_obj,
                                     &sort_obj, &skip, &limit)) {
        return nullptr;
    }
    std::vector<mimicapi::Filter> filters;
    if (!ParseFilters(filter_obj, &filters)) {
        return nullptr;
    }
    mimicapi::FindOptions options;
    if (!ParseSort(sort_obj, &options.sort)) {
        return nullptr;
    }
    if (skip > 0) {
        options.skip = static_cast<size_t>(skip);
    }
    if (limit > 0) {
        options.limit = static_cast<size_t>(limit);
    }
    mimicapi::ProjectionSpec projection;
    if (projection_obj != Py_None) {
        if (PyList_Check(projection_obj)) {
            const Py_ssize_t len = PyList_Size(projection_obj);
            projection.include.reserve(static_cast<size_t>(len));
            for (Py_ssize_t i = 0; i < len; ++i) {
                PyObject* item = PyList_GetItem(projection_obj, i);
                if (!PyUnicode_Check(item)) {
                    PyErr_SetString(PyExc_TypeError, "projection list must be strings");
                    return nullptr;
                }
                PyObject* bytes = PyUnicode_AsUTF8String(item);
                if (!bytes) {
                    return nullptr;
                }
                projection.include.emplace_back(PyBytes_AsString(bytes));
                Py_DECREF(bytes);
            }
        } else if (PyDict_Check(projection_obj)) {
            PyObject* key = nullptr;
            PyObject* value = nullptr;
            Py_ssize_t pos = 0;
            while (PyDict_Next(projection_obj, &pos, &key, &value)) {
                if (!PyUnicode_Check(key)) {
                    PyErr_SetString(PyExc_TypeError, "projection keys must be strings");
                    return nullptr;
                }
                PyObject* key_bytes = PyUnicode_AsUTF8String(key);
                if (!key_bytes) {
                    return nullptr;
                }
                std::string name = PyBytes_AsString(key_bytes);
                Py_DECREF(key_bytes);
                if (PyBool_Check(value) || PyLong_Check(value)) {
                    const long flag = PyLong_AsLong(value);
                    if (flag) {
                        projection.include.push_back(name);
                    } else {
                        projection.exclude.push_back(name);
                    }
                } else if (PyDict_Check(value)) {
                    PyObject* slice_obj = PyDict_GetItemString(value, "$slice");
                    if (slice_obj) {
                        mimicapi::SliceSpec slice;
                        slice.field = name;
                        if (PyLong_Check(slice_obj)) {
                            slice.has_limit = false;
                            slice.limit = static_cast<int32_t>(PyLong_AsLong(slice_obj));
                        } else if (PyList_Check(slice_obj) || PyTuple_Check(slice_obj)) {
                            PyObject* seq = PySequence_Fast(slice_obj, "$slice requires sequence");
                            if (!seq) {
                                return nullptr;
                            }
                            const Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
                            if (len != 2) {
                                Py_DECREF(seq);
                                PyErr_SetString(PyExc_TypeError, "$slice requires [skip, limit]");
                                return nullptr;
                            }
                            PyObject* skip_obj = PySequence_Fast_GET_ITEM(seq, 0);
                            PyObject* limit_obj = PySequence_Fast_GET_ITEM(seq, 1);
                            if (!PyLong_Check(skip_obj) || !PyLong_Check(limit_obj)) {
                                Py_DECREF(seq);
                                PyErr_SetString(PyExc_TypeError, "$slice values must be ints");
                                return nullptr;
                            }
                            slice.has_limit = true;
                            slice.skip = static_cast<int32_t>(PyLong_AsLong(skip_obj));
                            slice.limit = static_cast<int32_t>(PyLong_AsLong(limit_obj));
                            Py_DECREF(seq);
                        } else {
                            PyErr_SetString(PyExc_TypeError, "$slice must be int or list");
                            return nullptr;
                        }
                        projection.slices.push_back(std::move(slice));
                        continue;
                    }
                    PyObject* literal = PyDict_GetItemString(value, "$literal");
                    PyObject* field = PyDict_GetItemString(value, "$field");
                    mimicapi::ComputedField computed;
                    computed.name = name;
                    if (literal) {
                        computed.from_field = false;
                        computed.literal = ParseValue(literal);
                    } else if (field && PyUnicode_Check(field)) {
                        PyObject* field_bytes = PyUnicode_AsUTF8String(field);
                        if (!field_bytes) {
                            return nullptr;
                        }
                        computed.from_field = true;
                        computed.field = PyBytes_AsString(field_bytes);
                        Py_DECREF(field_bytes);
                    } else {
                        PyErr_SetString(PyExc_TypeError, "unsupported projection expression");
                        return nullptr;
                    }
                    projection.computed.push_back(std::move(computed));
                }
            }
        } else {
            PyErr_SetString(PyExc_TypeError, "projection must be list or dict");
            return nullptr;
        }
    }
    std::string error;
    const auto docs = self->mongo_core->Find(db, collection, filters, projection, options, &error);
    if (!error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(docs.size()));
    for (size_t i = 0; i < docs.size(); ++i) {
        PyObject* doc_dict = PyDict_New();
        for (const auto& item : docs[i].fields) {
            PyObject* py_value = FieldValueToPy(item.second);
            if (!py_value) {
                Py_DECREF(doc_dict);
                Py_DECREF(list);
                return nullptr;
            }
            PyDict_SetItemString(doc_dict, item.first.c_str(), py_value);
            Py_DECREF(py_value);
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), doc_dict);
    }
    return list;
}

PyObject* MongoClientUpdate(MongoClientCoreObject* self, PyObject* args, PyObject* kwargs) {
    const char* db = nullptr;
    const char* collection = nullptr;
    PyObject* filter_obj = Py_None;
    PyObject* update_obj = Py_None;
    int multi = 0;
    int upsert = 0;
    int replace = 0;
    static const char* kwlist[] = {"db", "collection", "filter", "update", "multi", "upsert",
                                   "replace", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ss|OOiii", const_cast<char**>(kwlist),
                                     &db, &collection, &filter_obj, &update_obj, &multi,
                                     &upsert, &replace)) {
        return nullptr;
    }
    if (!PyDict_Check(update_obj)) {
        PyErr_SetString(PyExc_TypeError, "update must be dict");
        return nullptr;
    }
    std::vector<mimicapi::Filter> filters;
    if (!ParseFilters(filter_obj, &filters)) {
        return nullptr;
    }
    mimicapi::UpdateSpec update;
    if (replace) {
        update.is_replacement = true;
        PyObject* key = nullptr;
        PyObject* value = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(update_obj, &pos, &key, &value)) {
            if (!PyUnicode_Check(key)) {
                PyErr_SetString(PyExc_TypeError, "field name must be string");
                return nullptr;
            }
            PyObject* key_bytes = PyUnicode_AsUTF8String(key);
            if (!key_bytes) {
                return nullptr;
            }
            std::string field = PyBytes_AsString(key_bytes);
            Py_DECREF(key_bytes);
            if (!field.empty() && field[0] == '$') {
                PyErr_SetString(PyExc_ValueError, "replacement documents cannot contain operators");
                return nullptr;
            }
            update.replacement[field] = ParseValue(value);
        }
    } else {
        PyObject* key = nullptr;
        PyObject* value = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(update_obj, &pos, &key, &value)) {
            if (!PyUnicode_Check(key)) {
                PyErr_SetString(PyExc_TypeError, "update operator must be string");
                return nullptr;
            }
            PyObject* key_bytes = PyUnicode_AsUTF8String(key);
            if (!key_bytes) {
                return nullptr;
            }
            std::string op = PyBytes_AsString(key_bytes);
            Py_DECREF(key_bytes);
            if (op.empty() || op[0] != '$') {
                PyErr_SetString(PyExc_ValueError, "update document must contain update operators");
                return nullptr;
            }
            if ((op == "$set" || op == "$setOnInsert" || op == "$rename" ||
                 op == "$currentDate" || op == "$unset" || op == "$push" ||
                 op == "$addToSet" || op == "$pull") && !value) {
                continue;
            }
            if (op == "$set" || op == "$setOnInsert") {
                if (!PyDict_Check(value)) {
                    PyErr_SetString(PyExc_TypeError, "update operator value must be dict");
                    return nullptr;
                }
                PyObject* field = nullptr;
                PyObject* field_value = nullptr;
                Py_ssize_t inner_pos = 0;
                while (PyDict_Next(value, &inner_pos, &field, &field_value)) {
                    if (!PyUnicode_Check(field)) {
                        PyErr_SetString(PyExc_TypeError, "field name must be string");
                        return nullptr;
                    }
                    PyObject* field_bytes = PyUnicode_AsUTF8String(field);
                    if (!field_bytes) {
                        return nullptr;
                    }
                    mimicapi::UpdateOp op_item;
                    op_item.type = (op == "$set")
                                       ? mimicapi::UpdateOpType::kSet
                                       : mimicapi::UpdateOpType::kSetOnInsert;
                    op_item.field = PyBytes_AsString(field_bytes);
                    Py_DECREF(field_bytes);
                    op_item.values.push_back(ParseValue(field_value));
                    update.ops.push_back(std::move(op_item));
                }
            } else if (op == "$rename") {
                if (!PyDict_Check(value)) {
                    PyErr_SetString(PyExc_TypeError, "$rename value must be dict");
                    return nullptr;
                }
                PyObject* field = nullptr;
                PyObject* field_value = nullptr;
                Py_ssize_t inner_pos = 0;
                while (PyDict_Next(value, &inner_pos, &field, &field_value)) {
                    if (!PyUnicode_Check(field) || !PyUnicode_Check(field_value)) {
                        PyErr_SetString(PyExc_TypeError, "$rename requires string mapping");
                        return nullptr;
                    }
                    PyObject* field_bytes = PyUnicode_AsUTF8String(field);
                    if (!field_bytes) {
                        return nullptr;
                    }
                    PyObject* rename_bytes = PyUnicode_AsUTF8String(field_value);
                    if (!rename_bytes) {
                        Py_DECREF(field_bytes);
                        return nullptr;
                    }
                    mimicapi::UpdateOp op_item;
                    op_item.type = mimicapi::UpdateOpType::kRename;
                    op_item.field = PyBytes_AsString(field_bytes);
                    op_item.rename_to = PyBytes_AsString(rename_bytes);
                    Py_DECREF(field_bytes);
                    Py_DECREF(rename_bytes);
                    update.ops.push_back(std::move(op_item));
                }
            } else if (op == "$currentDate") {
                if (!PyDict_Check(value)) {
                    PyErr_SetString(PyExc_TypeError, "$currentDate value must be dict");
                    return nullptr;
                }
                PyObject* field = nullptr;
                PyObject* field_value = nullptr;
                Py_ssize_t inner_pos = 0;
                while (PyDict_Next(value, &inner_pos, &field, &field_value)) {
                    if (!PyUnicode_Check(field)) {
                        PyErr_SetString(PyExc_TypeError, "field name must be string");
                        return nullptr;
                    }
                    mimicapi::UpdateOp op_item;
                    op_item.type = mimicapi::UpdateOpType::kCurrentDate;
                    PyObject* field_bytes = PyUnicode_AsUTF8String(field);
                    if (!field_bytes) {
                        return nullptr;
                    }
                    op_item.field = PyBytes_AsString(field_bytes);
                    Py_DECREF(field_bytes);
                    if (PyDict_Check(field_value)) {
                        PyObject* type_value = PyDict_GetItemString(field_value, "$type");
                        if (type_value && PyUnicode_Check(type_value)) {
                            PyObject* type_bytes = PyUnicode_AsUTF8String(type_value);
                            if (!type_bytes) {
                                return nullptr;
                            }
                            std::string type_str = PyBytes_AsString(type_bytes);
                            Py_DECREF(type_bytes);
                            op_item.current_timestamp = (type_str == "timestamp");
                        }
                    }
                    update.ops.push_back(std::move(op_item));
                }
            } else if (op == "$unset") {
                if (PyDict_Check(value)) {
                    PyObject* field = nullptr;
                    PyObject* field_value = nullptr;
                    Py_ssize_t inner_pos = 0;
                    while (PyDict_Next(value, &inner_pos, &field, &field_value)) {
                        if (!PyUnicode_Check(field)) {
                            PyErr_SetString(PyExc_TypeError, "field name must be string");
                            return nullptr;
                        }
                        PyObject* field_bytes = PyUnicode_AsUTF8String(field);
                        if (!field_bytes) {
                            return nullptr;
                        }
                        mimicapi::UpdateOp op_item;
                        op_item.type = mimicapi::UpdateOpType::kUnset;
                        op_item.field = PyBytes_AsString(field_bytes);
                        Py_DECREF(field_bytes);
                        update.ops.push_back(std::move(op_item));
                    }
                } else if (PyList_Check(value) || PyTuple_Check(value) || PySet_Check(value)) {
                    PyObject* seq = PySequence_Fast(value, "$unset list must be iterable");
                    if (!seq) {
                        return nullptr;
                    }
                    Py_ssize_t size = PySequence_Fast_GET_SIZE(seq);
                    for (Py_ssize_t i = 0; i < size; ++i) {
                        PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
                        if (!PyUnicode_Check(item)) {
                            Py_DECREF(seq);
                            PyErr_SetString(PyExc_TypeError, "$unset entries must be strings");
                            return nullptr;
                        }
                        PyObject* field_bytes = PyUnicode_AsUTF8String(item);
                        if (!field_bytes) {
                            Py_DECREF(seq);
                            return nullptr;
                        }
                        mimicapi::UpdateOp op_item;
                        op_item.type = mimicapi::UpdateOpType::kUnset;
                        op_item.field = PyBytes_AsString(field_bytes);
                        Py_DECREF(field_bytes);
                        update.ops.push_back(std::move(op_item));
                    }
                    Py_DECREF(seq);
                } else {
                    PyErr_SetString(PyExc_TypeError, "$unset value must be dict or list");
                    return nullptr;
                }
            } else if (op == "$push" || op == "$addToSet") {
                if (!PyDict_Check(value)) {
                    PyErr_SetString(PyExc_TypeError, "$push/$addToSet value must be dict");
                    return nullptr;
                }
                PyObject* field = nullptr;
                PyObject* field_value = nullptr;
                Py_ssize_t inner_pos = 0;
                while (PyDict_Next(value, &inner_pos, &field, &field_value)) {
                    if (!PyUnicode_Check(field)) {
                        PyErr_SetString(PyExc_TypeError, "field name must be string");
                        return nullptr;
                    }
                    PyObject* field_bytes = PyUnicode_AsUTF8String(field);
                    if (!field_bytes) {
                        return nullptr;
                    }
                    mimicapi::UpdateOp op_item;
                    op_item.type = (op == "$push")
                                       ? mimicapi::UpdateOpType::kPush
                                       : mimicapi::UpdateOpType::kAddToSet;
                    op_item.field = PyBytes_AsString(field_bytes);
                    Py_DECREF(field_bytes);
                    if (PyDict_Check(field_value)) {
                        PyObject* each_obj = PyDict_GetItemString(field_value, "$each");
                        if (each_obj) {
                            PyObject* seq = PySequence_Fast(each_obj, "$each must be iterable");
                            if (!seq) {
                                return nullptr;
                            }
                            Py_ssize_t size = PySequence_Fast_GET_SIZE(seq);
                            for (Py_ssize_t i = 0; i < size; ++i) {
                                PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
                                op_item.values.push_back(ParseValue(item));
                            }
                            Py_DECREF(seq);
                        } else {
                            op_item.values.push_back(ParseValue(field_value));
                        }
                    } else {
                        op_item.values.push_back(ParseValue(field_value));
                    }
                    update.ops.push_back(std::move(op_item));
                }
            } else if (op == "$pull") {
                if (!PyDict_Check(value)) {
                    PyErr_SetString(PyExc_TypeError, "$pull value must be dict");
                    return nullptr;
                }
                PyObject* field = nullptr;
                PyObject* field_value = nullptr;
                Py_ssize_t inner_pos = 0;
                while (PyDict_Next(value, &inner_pos, &field, &field_value)) {
                    if (!PyUnicode_Check(field)) {
                        PyErr_SetString(PyExc_TypeError, "field name must be string");
                        return nullptr;
                    }
                    PyObject* field_bytes = PyUnicode_AsUTF8String(field);
                    if (!field_bytes) {
                        return nullptr;
                    }
                    mimicapi::UpdateOp op_item;
                    op_item.type = mimicapi::UpdateOpType::kPull;
                    op_item.field = PyBytes_AsString(field_bytes);
                    Py_DECREF(field_bytes);
                    if (PyDict_Check(field_value)) {
                        PyObject* in_obj = PyDict_GetItemString(field_value, "$in");
                        PyObject* eq_obj = PyDict_GetItemString(field_value, "$eq");
                        PyObject* ne_obj = PyDict_GetItemString(field_value, "$ne");
                        PyObject* regex_obj = PyDict_GetItemString(field_value, "$regex");
                        if (in_obj) {
                            PyObject* seq = PySequence_Fast(in_obj, "$in must be iterable");
                            if (!seq) {
                                return nullptr;
                            }
                            Py_ssize_t size = PySequence_Fast_GET_SIZE(seq);
                            for (Py_ssize_t i = 0; i < size; ++i) {
                                PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
                                op_item.values.push_back(ParseValue(item));
                            }
                            Py_DECREF(seq);
                            op_item.pull_op = mimicapi::FilterOp::kIn;
                        } else if (eq_obj) {
                            op_item.values.push_back(ParseValue(eq_obj));
                            op_item.pull_op = mimicapi::FilterOp::kEq;
                        } else if (ne_obj) {
                            op_item.values.push_back(ParseValue(ne_obj));
                            op_item.pull_op = mimicapi::FilterOp::kNe;
                        } else if (regex_obj) {
                            if (!PyUnicode_Check(regex_obj)) {
                                PyErr_SetString(PyExc_TypeError, "$regex must be string");
                                return nullptr;
                            }
                            PyObject* regex_bytes = PyUnicode_AsUTF8String(regex_obj);
                            if (!regex_bytes) {
                                return nullptr;
                            }
                            op_item.regex = PyBytes_AsString(regex_bytes);
                            Py_DECREF(regex_bytes);
                            PyObject* options_obj = PyDict_GetItemString(field_value, "$options");
                            if (options_obj && PyUnicode_Check(options_obj)) {
                                PyObject* opt_bytes = PyUnicode_AsUTF8String(options_obj);
                                if (!opt_bytes) {
                                    return nullptr;
                                }
                                op_item.regex_options = PyBytes_AsString(opt_bytes);
                                Py_DECREF(opt_bytes);
                            }
                            op_item.pull_op = mimicapi::FilterOp::kRegex;
                        } else {
                            PyErr_SetString(PyExc_ValueError, "unsupported $pull operator");
                            return nullptr;
                        }
                    } else {
                        op_item.values.push_back(ParseValue(field_value));
                        op_item.pull_op = mimicapi::FilterOp::kEq;
                    }
                    update.ops.push_back(std::move(op_item));
                }
            } else {
                PyErr_SetString(PyExc_NotImplementedError, "unsupported update operator");
                return nullptr;
            }
        }
        if (update.ops.empty()) {
            PyErr_SetString(PyExc_ValueError, "update document must contain update operators");
            return nullptr;
        }
    }
    std::string error;
    const size_t matched = self->mongo_core->Update(db, collection, filters, update,
                                                    multi != 0, upsert != 0, replace != 0,
                                                    &error);
    if (!error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    return PyLong_FromUnsignedLongLong(matched);
}

PyObject* MongoClientDelete(MongoClientCoreObject* self, PyObject* args, PyObject* kwargs) {
    const char* db = nullptr;
    const char* collection = nullptr;
    PyObject* filter_obj = Py_None;
    int multi = 0;
    static const char* kwlist[] = {"db", "collection", "filter", "multi", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ss|Oi", const_cast<char**>(kwlist),
                                     &db, &collection, &filter_obj, &multi)) {
        return nullptr;
    }
    std::vector<mimicapi::Filter> filters;
    if (!ParseFilters(filter_obj, &filters)) {
        return nullptr;
    }
    std::string error;
    const size_t matched = self->mongo_core->Delete(db, collection, filters, multi != 0, &error);
    if (!error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    return PyLong_FromUnsignedLongLong(matched);
}

PyObject* MongoClientAggregate(MongoClientCoreObject* self, PyObject* args, PyObject* kwargs) {
    const char* db = nullptr;
    const char* collection = nullptr;
    PyObject* pipeline_obj = Py_None;
    static const char* kwlist[] = {"db", "collection", "pipeline", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ss|O", const_cast<char**>(kwlist),
                                     &db, &collection, &pipeline_obj)) {
        return nullptr;
    }
    std::vector<mimicapi::PipelineStage> pipeline;
    if (!ParsePipelineStages(pipeline_obj, &pipeline)) {
        return nullptr;
    }
    std::string error;
    const auto docs = self->mongo_core->AggregatePipeline(db, collection, pipeline, &error);
    if (!error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(docs.size()));
    for (size_t i = 0; i < docs.size(); ++i) {
        PyObject* doc_dict = PyDict_New();
        for (const auto& item : docs[i].fields) {
            PyObject* py_value = FieldValueToPy(item.second);
            if (!py_value) {
                Py_DECREF(doc_dict);
                Py_DECREF(list);
                return nullptr;
            }
            PyDict_SetItemString(doc_dict, item.first.c_str(), py_value);
            Py_DECREF(py_value);
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), doc_dict);
    }
    return list;
}

PyMethodDef MongoClientMethods[] = {
    {"insert_many", (PyCFunction)MongoClientInsertMany, METH_VARARGS, nullptr},
    {"find", (PyCFunction)MongoClientFind, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"update", (PyCFunction)MongoClientUpdate, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"delete", (PyCFunction)MongoClientDelete, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"aggregate", (PyCFunction)MongoClientAggregate, METH_VARARGS | METH_KEYWORDS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

PyTypeObject MongoClientCoreType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
};

PyModuleDef ModuleDef = {
    PyModuleDef_HEAD_INIT,
    "mimicapi._mimicapi_mongo",
    nullptr,
    -1,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

}  // namespace

PyMODINIT_FUNC PyInit__mimicapi_mongo(void) {
    MongoClientCoreType.tp_name = "mimicapi._mimicapi_mongo.MongoClientCore";
    MongoClientCoreType.tp_basicsize = sizeof(MongoClientCoreObject);
    MongoClientCoreType.tp_flags = Py_TPFLAGS_DEFAULT;
    MongoClientCoreType.tp_new = PyType_GenericNew;
    MongoClientCoreType.tp_init = (initproc)MongoClientCoreInit;
    MongoClientCoreType.tp_dealloc = (destructor)MongoClientCoreDealloc;
    MongoClientCoreType.tp_methods = MongoClientMethods;
    if (PyType_Ready(&MongoClientCoreType) < 0) {
        return nullptr;
    }
    PyObject* module = PyModule_Create(&ModuleDef);
    if (!module) {
        return nullptr;
    }
    Py_INCREF(&MongoClientCoreType);
    PyModule_AddObject(module, "MongoClientCore", reinterpret_cast<PyObject*>(&MongoClientCoreType));
    return module;
}
