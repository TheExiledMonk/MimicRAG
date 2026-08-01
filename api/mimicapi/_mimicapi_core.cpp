#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "mimicapi/core.h"
#include "mimicdb/types.h"
#include "mimicdb/compression.h"

namespace {

struct ApiClientCoreObject {
    PyObject_HEAD
    mimicapi::ApiClientCore* core = nullptr;
};

mimicdb::FieldType ParseFieldType(const std::string& type_name, bool* ok) {
    *ok = true;
    if (type_name == "int32") {
        return mimicdb::FieldType::kInt32;
    }
    if (type_name == "int64") {
        return mimicdb::FieldType::kInt64;
    }
    if (type_name == "float64") {
        return mimicdb::FieldType::kFloat64;
    }
    if (type_name == "bool") {
        return mimicdb::FieldType::kBool;
    }
    if (type_name == "string") {
        return mimicdb::FieldType::kString;
    }
    if (type_name == "bytes") {
        return mimicdb::FieldType::kBytes;
    }
    if (type_name == "array") {
        return mimicdb::FieldType::kArray;
    }
    if (type_name == "dict_int32") {
        return mimicdb::FieldType::kDictInt32;
    }
    *ok = false;
    return mimicdb::FieldType::kInt32;
}

bool ParseCompareOp(const std::string& op, mimicdb::CompareOp* out) {
    if (op == "eq") {
        *out = mimicdb::CompareOp::kEq;
        return true;
    }
    if (op == "ne") {
        *out = mimicdb::CompareOp::kNe;
        return true;
    }
    if (op == "lt") {
        *out = mimicdb::CompareOp::kLt;
        return true;
    }
    if (op == "le") {
        *out = mimicdb::CompareOp::kLe;
        return true;
    }
    if (op == "gt") {
        *out = mimicdb::CompareOp::kGt;
        return true;
    }
    if (op == "ge") {
        *out = mimicdb::CompareOp::kGe;
        return true;
    }
    return false;
}

int ApiClientCoreInit(ApiClientCoreObject* self, PyObject*, PyObject*) {
    self->core = new mimicapi::ApiClientCore();
    return 0;
}

void ApiClientCoreDealloc(ApiClientCoreObject* self) {
    delete self->core;
    self->core = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

PyObject* ApiClientCoreCreateDatabase(ApiClientCoreObject* self, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s", &name)) {
        return nullptr;
    }
    const bool ok = self->core->CreateDatabase(name);
    if (!ok) {
        PyErr_SetString(PyExc_RuntimeError, "create_database failed");
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* ApiClientCoreCreateDataset(ApiClientCoreObject* self, PyObject* args) {
    const char* db = nullptr;
    const char* name = nullptr;
    PyObject* fields_obj = nullptr;
    if (!PyArg_ParseTuple(args, "ssO", &db, &name, &fields_obj)) {
        return nullptr;
    }
    if (!PyDict_Check(fields_obj)) {
        PyErr_SetString(PyExc_TypeError, "fields must be a dict");
        return nullptr;
    }
    std::vector<mimicapi::FieldDef> fields;
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(fields_obj, &pos, &key, &value)) {
        if (!PyUnicode_Check(key) || !PyUnicode_Check(value)) {
            PyErr_SetString(PyExc_TypeError, "field names and types must be strings");
            return nullptr;
        }
        PyObject* key_bytes = PyUnicode_AsUTF8String(key);
        PyObject* value_bytes = PyUnicode_AsUTF8String(value);
        if (!key_bytes || !value_bytes) {
            Py_XDECREF(key_bytes);
            Py_XDECREF(value_bytes);
            return nullptr;
        }
        std::string field_name = PyBytes_AsString(key_bytes);
        std::string type_name = PyBytes_AsString(value_bytes);
        Py_DECREF(key_bytes);
        Py_DECREF(value_bytes);
        bool ok = false;
        const auto type = ParseFieldType(type_name, &ok);
        if (!ok) {
            PyErr_SetString(PyExc_ValueError, "unsupported field type");
            return nullptr;
        }
        fields.push_back({field_name, type});
    }
    const bool ok = self->core->CreateDataset(db, name, fields);
    if (!ok) {
        PyErr_SetString(PyExc_RuntimeError, "create_dataset failed");
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* ApiClientCoreDropDatabase(ApiClientCoreObject* self, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s", &name)) {
        return nullptr;
    }
    const bool ok = self->core->DropDatabase(name);
    if (!ok) {
        PyErr_SetString(PyExc_RuntimeError, "drop_database failed");
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* ApiClientCoreDropDataset(ApiClientCoreObject* self, PyObject* args) {
    const char* db = nullptr;
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "ss", &db, &name)) {
        return nullptr;
    }
    const bool ok = self->core->DropDataset(db, name);
    if (!ok) {
        PyErr_SetString(PyExc_RuntimeError, "drop_dataset failed");
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* ApiClientCoreAppendBatch(ApiClientCoreObject* self, PyObject* args) {
    const char* db = nullptr;
    const char* name = nullptr;
    PyObject* columns_obj = nullptr;
    if (!PyArg_ParseTuple(args, "ssO", &db, &name, &columns_obj)) {
        return nullptr;
    }
    if (!PyDict_Check(columns_obj)) {
        PyErr_SetString(PyExc_TypeError, "columns must be a dict");
        return nullptr;
    }
    const auto* field_defs = self->core->FieldsFor(db, name);
    if (!field_defs) {
        PyErr_SetString(PyExc_KeyError, "unknown dataset");
        return nullptr;
    }
    if (static_cast<size_t>(PyDict_Size(columns_obj)) != field_defs->size()) {
        PyErr_SetString(PyExc_ValueError, "append_batch requires all fields");
        return nullptr;
    }
    for (const auto& field : *field_defs) {
        if (!PyDict_GetItemString(columns_obj, field.name.c_str())) {
            PyErr_SetString(PyExc_ValueError, "append_batch requires all fields");
            return nullptr;
        }
    }
    size_t count = 0;
    bool first = true;

    std::vector<std::vector<int32_t>> i32_values;
    std::vector<std::vector<int64_t>> i64_values;
    std::vector<std::vector<double>> f64_values;
    std::vector<std::vector<uint8_t>> bool_values;
    std::vector<std::vector<uint32_t>> length_values;
    std::vector<std::vector<uint8_t>> bytes_values;
    std::vector<std::vector<uint8_t>> validity_buffers;

    std::vector<mimicdb::FieldBatch> batches;
    batches.reserve(field_defs->size());

    for (const auto& field : *field_defs) {
        PyObject* column = PyDict_GetItemString(columns_obj, field.name.c_str());
        if (!column) {
            PyErr_SetString(PyExc_ValueError, "append_batch requires all fields");
            return nullptr;
        }
        PyObject* seq = PySequence_Fast(column, "column must be a sequence");
        if (!seq) {
            return nullptr;
        }
        const Py_ssize_t seq_len = PySequence_Fast_GET_SIZE(seq);
        if (first) {
            count = static_cast<size_t>(seq_len);
            first = false;
        } else if (static_cast<size_t>(seq_len) != count) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_ValueError, "all columns must have same length");
            return nullptr;
        }
        mimicdb::FieldBatch batch;
        batch.type = field.type;
        batch.count = count;
        std::vector<uint8_t> validity;
        validity.reserve(count);

        if (batch.type == mimicdb::FieldType::kInt32 || batch.type == mimicdb::FieldType::kDictInt32) {
            std::vector<int32_t> values;
            values.reserve(count);
            for (Py_ssize_t j = 0; j < seq_len; ++j) {
                PyObject* item = PySequence_Fast_GET_ITEM(seq, j);
                if (item == Py_None) {
                    values.push_back(0);
                    validity.push_back(0);
                    continue;
                }
                if (!PyLong_Check(item) || PyBool_Check(item)) {
                    Py_DECREF(seq);
                    PyErr_SetString(PyExc_TypeError, "int32 field requires int");
                    return nullptr;
                }
                const long long val = PyLong_AsLongLong(item);
                if (PyErr_Occurred()) {
                    Py_DECREF(seq);
                    return nullptr;
                }
                values.push_back(static_cast<int32_t>(val));
                validity.push_back(1);
            }
            i32_values.push_back(std::move(values));
            batch.data = i32_values.back().data();
        } else if (batch.type == mimicdb::FieldType::kInt64) {
            std::vector<int64_t> values;
            values.reserve(count);
            for (Py_ssize_t j = 0; j < seq_len; ++j) {
                PyObject* item = PySequence_Fast_GET_ITEM(seq, j);
                if (item == Py_None) {
                    values.push_back(0);
                    validity.push_back(0);
                    continue;
                }
                if (!PyLong_Check(item) || PyBool_Check(item)) {
                    Py_DECREF(seq);
                    PyErr_SetString(PyExc_TypeError, "int64 field requires int");
                    return nullptr;
                }
                const long long val = PyLong_AsLongLong(item);
                if (PyErr_Occurred()) {
                    Py_DECREF(seq);
                    return nullptr;
                }
                values.push_back(static_cast<int64_t>(val));
                validity.push_back(1);
            }
            i64_values.push_back(std::move(values));
            batch.data = i64_values.back().data();
        } else if (batch.type == mimicdb::FieldType::kFloat64) {
            std::vector<double> values;
            values.reserve(count);
            for (Py_ssize_t j = 0; j < seq_len; ++j) {
                PyObject* item = PySequence_Fast_GET_ITEM(seq, j);
                if (item == Py_None) {
                    values.push_back(0.0);
                    validity.push_back(0);
                    continue;
                }
                if ((!PyFloat_Check(item) && !PyLong_Check(item)) || PyBool_Check(item)) {
                    Py_DECREF(seq);
                    PyErr_SetString(PyExc_TypeError, "float64 field requires float");
                    return nullptr;
                }
                const double val = PyFloat_AsDouble(item);
                if (PyErr_Occurred()) {
                    Py_DECREF(seq);
                    return nullptr;
                }
                values.push_back(val);
                validity.push_back(1);
            }
            f64_values.push_back(std::move(values));
            batch.data = f64_values.back().data();
        } else if (batch.type == mimicdb::FieldType::kBool) {
            std::vector<uint8_t> values;
            values.reserve(count);
            for (Py_ssize_t j = 0; j < seq_len; ++j) {
                PyObject* item = PySequence_Fast_GET_ITEM(seq, j);
                if (item == Py_None) {
                    values.push_back(0);
                    validity.push_back(0);
                    continue;
                }
                if (!PyBool_Check(item)) {
                    Py_DECREF(seq);
                    PyErr_SetString(PyExc_TypeError, "bool field requires bool");
                    return nullptr;
                }
                const int truth = PyObject_IsTrue(item);
                if (truth < 0) {
                    Py_DECREF(seq);
                    return nullptr;
                }
                values.push_back(truth ? 1 : 0);
                validity.push_back(1);
            }
            bool_values.push_back(std::move(values));
            batch.data = bool_values.back().data();
        } else if (batch.type == mimicdb::FieldType::kString || batch.type == mimicdb::FieldType::kBytes) {
            std::vector<uint32_t> lengths;
            std::vector<uint8_t> bytes;
            lengths.reserve(count);
            for (Py_ssize_t j = 0; j < seq_len; ++j) {
                PyObject* item = PySequence_Fast_GET_ITEM(seq, j);
                if (item == Py_None) {
                    lengths.push_back(0);
                    validity.push_back(0);
                    continue;
                }
                PyObject* as_bytes = nullptr;
                if (batch.type == mimicdb::FieldType::kString) {
                    if (!PyUnicode_Check(item)) {
                        Py_DECREF(seq);
                        PyErr_SetString(PyExc_TypeError, "string field requires str");
                        return nullptr;
                    }
                    as_bytes = PyUnicode_AsUTF8String(item);
                } else {
                    if (!PyBytes_Check(item)) {
                        Py_DECREF(seq);
                        PyErr_SetString(PyExc_TypeError, "bytes field requires bytes");
                        return nullptr;
                    }
                    as_bytes = item;
                    Py_INCREF(as_bytes);
                }
                if (!as_bytes) {
                    Py_DECREF(seq);
                    return nullptr;
                }
                Py_ssize_t size = 0;
                char* data = nullptr;
                if (PyBytes_AsStringAndSize(as_bytes, &data, &size) != 0) {
                    Py_DECREF(seq);
                    Py_DECREF(as_bytes);
                    return nullptr;
                }
                lengths.push_back(static_cast<uint32_t>(size));
                bytes.insert(bytes.end(), data, data + size);
                validity.push_back(1);
                Py_DECREF(as_bytes);
            }
            length_values.push_back(std::move(lengths));
            bytes_values.push_back(std::move(bytes));
            batch.lengths = length_values.back().data();
            batch.bytes = bytes_values.back().data();
            batch.bytes_size = bytes_values.back().size();
        } else {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_ValueError, "unsupported field type");
            return nullptr;
        }
        validity_buffers.push_back(std::move(validity));
        batch.validity = validity_buffers.back().data();
        batches.push_back(batch);
        Py_DECREF(seq);
    }
    std::string error;
    if (!self->core->AppendBatch(db, name, batches, &error)) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

bool ParsePredicates(PyObject* list_obj, std::vector<mimicapi::Predicate>* out) {
    if (!list_obj || list_obj == Py_None) {
        return true;
    }
    PyObject* seq = PySequence_Fast(list_obj, "predicates must be a sequence");
    if (!seq) {
        return false;
    }
    const Py_ssize_t seq_len = PySequence_Fast_GET_SIZE(seq);
    out->reserve(static_cast<size_t>(seq_len));
    for (Py_ssize_t i = 0; i < seq_len; ++i) {
        PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
        if (!PyTuple_Check(item) || PyTuple_Size(item) != 3) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_TypeError, "predicate must be (index, op, value)");
            return false;
        }
        auto* index_obj = PyTuple_GetItem(item, 0);
        auto* op_obj = PyTuple_GetItem(item, 1);
        auto* value_obj = PyTuple_GetItem(item, 2);
        if (!PyLong_Check(index_obj) || !PyUnicode_Check(op_obj)) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_TypeError, "predicate types invalid");
            return false;
        }
        mimicapi::Predicate pred;
        pred.field_index = static_cast<size_t>(PyLong_AsUnsignedLong(index_obj));
        PyObject* op_bytes = PyUnicode_AsUTF8String(op_obj);
        if (!op_bytes) {
            Py_DECREF(seq);
            return false;
        }
        std::string op = PyBytes_AsString(op_bytes);
        Py_DECREF(op_bytes);
        if (op == "is_null" || op == "is_not_null") {
            pred.is_null_check = true;
            pred.null_is = (op == "is_null");
        } else {
            if (!ParseCompareOp(op, &pred.op)) {
                Py_DECREF(seq);
                PyErr_SetString(PyExc_ValueError, "unsupported predicate op");
                return false;
            }
            if (PyUnicode_Check(value_obj)) {
                PyObject* value_bytes = PyUnicode_AsUTF8String(value_obj);
                if (!value_bytes) {
                    Py_DECREF(seq);
                    return false;
                }
                pred.bytes = PyBytes_AsString(value_bytes);
                pred.value_type = mimicdb::FieldType::kString;
                Py_DECREF(value_bytes);
            } else if (PyBytes_Check(value_obj)) {
                char* data = nullptr;
                Py_ssize_t size = 0;
                if (PyBytes_AsStringAndSize(value_obj, &data, &size) != 0) {
                    Py_DECREF(seq);
                    return false;
                }
                pred.bytes.assign(data, static_cast<size_t>(size));
                pred.value_type = mimicdb::FieldType::kBytes;
            } else {
                pred.value = PyFloat_AsDouble(value_obj);
                if (PyErr_Occurred()) {
                    Py_DECREF(seq);
                    return false;
                }
                pred.value_type = mimicdb::FieldType::kFloat64;
            }
        }
        out->push_back(pred);
    }
    Py_DECREF(seq);
    return true;
}

bool ValidatePredicates(const std::vector<mimicapi::FieldDef>& fields,
                        const std::vector<mimicapi::Predicate>& predicates) {
    for (const auto& pred : predicates) {
        if (pred.field_index >= fields.size()) {
            PyErr_SetString(PyExc_IndexError, "predicate field_index out of range");
            return false;
        }
        const auto field_type = fields[pred.field_index].type;
        if (pred.is_null_check) {
            continue;
        }
        if (pred.value_type == mimicdb::FieldType::kString ||
            pred.value_type == mimicdb::FieldType::kBytes) {
            if (field_type != pred.value_type) {
                PyErr_SetString(PyExc_TypeError, "predicate type mismatch");
                return false;
            }
            if (!(pred.op == mimicdb::CompareOp::kEq || pred.op == mimicdb::CompareOp::kNe)) {
                PyErr_SetString(PyExc_ValueError, "unsupported predicate op for string/bytes");
                return false;
            }
        }
    }
    return true;
}

bool ParseAggregateRequests(PyObject* list_obj,
                            std::vector<mimicapi::AggregateRequest>* out) {
    if (!list_obj || list_obj == Py_None) {
        PyErr_SetString(PyExc_TypeError, "aggregate requests must be a sequence");
        return false;
    }
    PyObject* seq = PySequence_Fast(list_obj, "aggregate requests must be a sequence");
    if (!seq) {
        return false;
    }
    const Py_ssize_t seq_len = PySequence_Fast_GET_SIZE(seq);
    out->reserve(static_cast<size_t>(seq_len));
    for (Py_ssize_t i = 0; i < seq_len; ++i) {
        PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
        if (!PyDict_Check(item)) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_TypeError, "aggregate request must be a dict");
            return false;
        }
        PyObject* kind_obj = PyDict_GetItemString(item, "kind");
        PyObject* field_obj = PyDict_GetItemString(item, "field_index");
        PyObject* alias_obj = PyDict_GetItemString(item, "alias");
        if (!kind_obj || !PyUnicode_Check(kind_obj) || !field_obj || !PyLong_Check(field_obj)) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_TypeError, "aggregate request requires kind and field_index");
            return false;
        }
        PyObject* kind_bytes = PyUnicode_AsUTF8String(kind_obj);
        if (!kind_bytes) {
            Py_DECREF(seq);
            return false;
        }
        std::string kind = PyBytes_AsString(kind_bytes);
        Py_DECREF(kind_bytes);
        mimicapi::AggregateRequest req;
        if (kind == "COUNT") {
            req.kind = mimicapi::AggregateKind::kCount;
        } else if (kind == "SUM") {
            req.kind = mimicapi::AggregateKind::kSum;
        } else if (kind == "MIN") {
            req.kind = mimicapi::AggregateKind::kMin;
        } else if (kind == "MAX") {
            req.kind = mimicapi::AggregateKind::kMax;
        } else {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_ValueError, "unsupported aggregate kind");
            return false;
        }
        req.field_index = static_cast<size_t>(PyLong_AsUnsignedLong(field_obj));
        if (alias_obj && PyUnicode_Check(alias_obj)) {
            PyObject* alias_bytes = PyUnicode_AsUTF8String(alias_obj);
            if (!alias_bytes) {
                Py_DECREF(seq);
                return false;
            }
            req.alias = PyBytes_AsString(alias_bytes);
            Py_DECREF(alias_bytes);
        }
        out->push_back(std::move(req));
    }
    Py_DECREF(seq);
    return true;
}

PyObject* ApiClientCoreScan(ApiClientCoreObject* self, PyObject* args, PyObject* kwargs) {
    const char* db = nullptr;
    const char* name = nullptr;
    PyObject* columns_obj = Py_None;
    PyObject* predicates_obj = Py_None;
    unsigned long long limit = 0;
    unsigned long long offset = 0;
    static const char* kwlist[] = {"db", "name", "columns", "predicates", "limit", "offset", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ss|OOkk",
                                     const_cast<char**>(kwlist),
                                     &db, &name, &columns_obj, &predicates_obj,
                                     &limit, &offset)) {
        return nullptr;
    }
    std::vector<std::string> columns;
    if (columns_obj != Py_None) {
        PyObject* seq = PySequence_Fast(columns_obj, "columns must be a sequence");
        if (!seq) {
            return nullptr;
        }
        const Py_ssize_t seq_len = PySequence_Fast_GET_SIZE(seq);
        columns.reserve(static_cast<size_t>(seq_len));
        for (Py_ssize_t i = 0; i < seq_len; ++i) {
            PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
            if (!PyUnicode_Check(item)) {
                Py_DECREF(seq);
                PyErr_SetString(PyExc_TypeError, "column names must be strings");
                return nullptr;
            }
            PyObject* bytes = PyUnicode_AsUTF8String(item);
            if (!bytes) {
                Py_DECREF(seq);
                return nullptr;
            }
            columns.emplace_back(PyBytes_AsString(bytes));
            Py_DECREF(bytes);
        }
        Py_DECREF(seq);
    }
    std::vector<mimicapi::Predicate> predicates;
    if (!ParsePredicates(predicates_obj, &predicates)) {
        return nullptr;
    }
    const auto* field_defs = self->core->FieldsFor(db, name);
    if (!field_defs) {
        PyErr_SetString(PyExc_KeyError, "unknown dataset");
        return nullptr;
    }
    if (!ValidatePredicates(*field_defs, predicates)) {
        return nullptr;
    }
    std::string error;
    const auto result = self->core->Scan(db, name, columns, predicates,
                                         static_cast<size_t>(limit),
                                         static_cast<size_t>(offset), &error);
    if (!error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    PyObject* rows_list = PyList_New(static_cast<Py_ssize_t>(result.rows.size()));
    for (size_t i = 0; i < result.rows.size(); ++i) {
        PyObject* row_dict = PyDict_New();
        for (size_t j = 0; j < result.columns.size(); ++j) {
            const auto& value = result.rows[i][j];
            PyObject* py_value = Py_None;
            if (value.is_null) {
                Py_INCREF(Py_None);
                PyDict_SetItemString(row_dict, result.columns[j].c_str(), Py_None);
                continue;
            }
            switch (value.type) {
                case mimicdb::FieldType::kInt32:
                    py_value = PyLong_FromLong(value.i32);
                    break;
                case mimicdb::FieldType::kInt64:
                    py_value = PyLong_FromLongLong(value.i64);
                    break;
                case mimicdb::FieldType::kFloat64:
                    py_value = PyFloat_FromDouble(value.f64);
                    break;
                case mimicdb::FieldType::kBool:
                    py_value = PyBool_FromLong(value.b ? 1 : 0);
                    break;
                case mimicdb::FieldType::kDictInt32:
                    py_value = PyLong_FromLong(value.i32);
                    break;
                case mimicdb::FieldType::kString:
                    py_value = PyUnicode_FromStringAndSize(value.bytes.data(),
                                                           static_cast<Py_ssize_t>(value.bytes.size()));
                    break;
                case mimicdb::FieldType::kBytes:
                    py_value = PyBytes_FromStringAndSize(value.bytes.data(),
                                                         static_cast<Py_ssize_t>(value.bytes.size()));
                    break;
            }
            if (!py_value) {
                Py_DECREF(row_dict);
                Py_DECREF(rows_list);
                return nullptr;
            }
            PyDict_SetItemString(row_dict, result.columns[j].c_str(), py_value);
            Py_DECREF(py_value);
        }
        PyList_SET_ITEM(rows_list, static_cast<Py_ssize_t>(i), row_dict);
    }
    return rows_list;
}

PyObject* ApiClientCoreScanDebug(ApiClientCoreObject* self, PyObject* args, PyObject* kwargs) {
    const char* db = nullptr;
    const char* name = nullptr;
    PyObject* columns_obj = Py_None;
    PyObject* predicates_obj = Py_None;
    unsigned long long limit = 0;
    unsigned long long offset = 0;
    static const char* kwlist[] = {"db", "name", "columns", "predicates", "limit", "offset", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ss|OOkk",
                                     const_cast<char**>(kwlist),
                                     &db, &name, &columns_obj, &predicates_obj,
                                     &limit, &offset)) {
        return nullptr;
    }
    std::vector<std::string> columns;
    if (columns_obj != Py_None) {
        PyObject* seq = PySequence_Fast(columns_obj, "columns must be a sequence");
        if (!seq) {
            return nullptr;
        }
        const Py_ssize_t seq_len = PySequence_Fast_GET_SIZE(seq);
        columns.reserve(static_cast<size_t>(seq_len));
        for (Py_ssize_t i = 0; i < seq_len; ++i) {
            PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
            if (!PyUnicode_Check(item)) {
                Py_DECREF(seq);
                PyErr_SetString(PyExc_TypeError, "column names must be strings");
                return nullptr;
            }
            PyObject* bytes = PyUnicode_AsUTF8String(item);
            if (!bytes) {
                Py_DECREF(seq);
                return nullptr;
            }
            columns.emplace_back(PyBytes_AsString(bytes));
            Py_DECREF(bytes);
        }
        Py_DECREF(seq);
    }
    std::vector<mimicapi::Predicate> predicates;
    if (!ParsePredicates(predicates_obj, &predicates)) {
        return nullptr;
    }
    const auto* field_defs = self->core->FieldsFor(db, name);
    if (!field_defs) {
        PyErr_SetString(PyExc_KeyError, "unknown dataset");
        return nullptr;
    }
    if (!ValidatePredicates(*field_defs, predicates)) {
        return nullptr;
    }
    std::string error;
    const auto result = self->core->Scan(db, name, columns, predicates,
                                         static_cast<size_t>(limit),
                                         static_cast<size_t>(offset), &error);
    if (!error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    PyObject* rows_list = PyList_New(static_cast<Py_ssize_t>(result.rows.size()));
    for (size_t i = 0; i < result.rows.size(); ++i) {
        PyObject* row_dict = PyDict_New();
        for (size_t j = 0; j < result.columns.size(); ++j) {
            const auto& value = result.rows[i][j];
            PyObject* py_value = Py_None;
            if (value.is_null) {
                Py_INCREF(Py_None);
                PyDict_SetItemString(row_dict, result.columns[j].c_str(), Py_None);
                continue;
            }
            switch (value.type) {
                case mimicdb::FieldType::kInt32:
                    py_value = PyLong_FromLong(value.i32);
                    break;
                case mimicdb::FieldType::kInt64:
                    py_value = PyLong_FromLongLong(value.i64);
                    break;
                case mimicdb::FieldType::kFloat64:
                    py_value = PyFloat_FromDouble(value.f64);
                    break;
                case mimicdb::FieldType::kBool:
                    py_value = PyBool_FromLong(value.b ? 1 : 0);
                    break;
                case mimicdb::FieldType::kDictInt32:
                    py_value = PyLong_FromLong(value.i32);
                    break;
                case mimicdb::FieldType::kString:
                    py_value = PyUnicode_FromStringAndSize(
                        value.bytes.data(),
                        static_cast<Py_ssize_t>(value.bytes.size()));
                    break;
                case mimicdb::FieldType::kBytes:
                    py_value = PyBytes_FromStringAndSize(
                        value.bytes.data(),
                        static_cast<Py_ssize_t>(value.bytes.size()));
                    break;
            }
            if (!py_value) {
                Py_DECREF(row_dict);
                Py_DECREF(rows_list);
                return nullptr;
            }
            PyDict_SetItemString(row_dict, result.columns[j].c_str(), py_value);
            Py_DECREF(py_value);
        }
        PyList_SET_ITEM(rows_list, static_cast<Py_ssize_t>(i), row_dict);
    }
    PyObject* stats = PyDict_New();
    PyDict_SetItemString(stats, "rows_scanned",
                         PyLong_FromUnsignedLongLong(result.rows_scanned));
    PyDict_SetItemString(stats, "rows_pruned",
                         PyLong_FromUnsignedLongLong(result.rows_pruned));
    return PyTuple_Pack(2, rows_list, stats);
}

PyObject* ApiClientCoreAggregate(ApiClientCoreObject* self, PyObject* args, PyObject* kwargs) {
    const char* db = nullptr;
    const char* name = nullptr;
    size_t field_index = 0;
    PyObject* predicates_obj = Py_None;
    static const char* kwlist[] = {"db", "name", "field_index", "predicates", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ssn|O",
                                     const_cast<char**>(kwlist),
                                     &db, &name, &field_index, &predicates_obj)) {
        return nullptr;
    }
    std::vector<mimicapi::Predicate> predicates;
    if (!ParsePredicates(predicates_obj, &predicates)) {
        return nullptr;
    }
    const auto* field_defs = self->core->FieldsFor(db, name);
    if (!field_defs) {
        PyErr_SetString(PyExc_KeyError, "unknown dataset");
        return nullptr;
    }
    if (!ValidatePredicates(*field_defs, predicates)) {
        return nullptr;
    }
    if (field_index >= field_defs->size()) {
        PyErr_SetString(PyExc_IndexError, "field_index out of range");
        return nullptr;
    }
    const auto field_type = (*field_defs)[field_index].type;
    if (field_type != mimicdb::FieldType::kInt32 &&
        field_type != mimicdb::FieldType::kInt64 &&
        field_type != mimicdb::FieldType::kFloat64 &&
        field_type != mimicdb::FieldType::kBool &&
        field_type != mimicdb::FieldType::kDictInt32) {
        PyErr_SetString(PyExc_TypeError, "aggregate requires numeric field");
        return nullptr;
    }
    std::string error;
    const auto result = self->core->Aggregate(db, name, field_index, predicates, &error);
    if (!error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    PyObject* dict = PyDict_New();
    PyDict_SetItemString(dict, "count", PyLong_FromUnsignedLongLong(result.count));
    PyDict_SetItemString(dict, "sum", PyFloat_FromDouble(result.sum));
    if (result.has_value) {
        PyDict_SetItemString(dict, "min", PyFloat_FromDouble(result.min));
        PyDict_SetItemString(dict, "max", PyFloat_FromDouble(result.max));
    } else {
        Py_INCREF(Py_None);
        PyDict_SetItemString(dict, "min", Py_None);
        Py_INCREF(Py_None);
        PyDict_SetItemString(dict, "max", Py_None);
    }
    PyDict_SetItemString(dict, "rows_scanned",
                         PyLong_FromUnsignedLongLong(result.rows_scanned));
    PyDict_SetItemString(dict, "rows_pruned",
                         PyLong_FromUnsignedLongLong(result.rows_pruned));
    return dict;
}

PyObject* ApiClientCoreAggregateMulti(ApiClientCoreObject* self, PyObject* args, PyObject* kwargs) {
    const char* db = nullptr;
    const char* name = nullptr;
    PyObject* requests_obj = nullptr;
    PyObject* predicates_obj = Py_None;
    static const char* kwlist[] = {"db", "name", "requests", "predicates", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ssO|O",
                                     const_cast<char**>(kwlist),
                                     &db, &name, &requests_obj, &predicates_obj)) {
        return nullptr;
    }
    std::vector<mimicapi::AggregateRequest> requests;
    if (!ParseAggregateRequests(requests_obj, &requests)) {
        return nullptr;
    }
    std::vector<mimicapi::Predicate> predicates;
    if (!ParsePredicates(predicates_obj, &predicates)) {
        return nullptr;
    }
    const auto* field_defs = self->core->FieldsFor(db, name);
    if (!field_defs) {
        PyErr_SetString(PyExc_KeyError, "unknown dataset");
        return nullptr;
    }
    if (!ValidatePredicates(*field_defs, predicates)) {
        return nullptr;
    }
    std::string error;
    const auto result = self->core->AggregateMulti(db, name, requests, predicates, &error);
    if (!error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    PyObject* dict = PyDict_New();
    for (size_t i = 0; i < result.requests.size(); ++i) {
        const auto& req = result.requests[i];
        const auto& agg = result.results[i];
        std::string key;
        if (!req.alias.empty()) {
            key = req.alias;
        } else {
            switch (req.kind) {
                case mimicapi::AggregateKind::kCount:
                    key = "count";
                    break;
                case mimicapi::AggregateKind::kSum:
                    key = "sum_" + std::to_string(req.field_index);
                    break;
                case mimicapi::AggregateKind::kMin:
                    key = "min_" + std::to_string(req.field_index);
                    break;
                case mimicapi::AggregateKind::kMax:
                    key = "max_" + std::to_string(req.field_index);
                    break;
            }
        }
        PyObject* value = nullptr;
        switch (req.kind) {
            case mimicapi::AggregateKind::kCount:
                value = PyLong_FromUnsignedLongLong(agg.count);
                break;
            case mimicapi::AggregateKind::kSum:
                value = PyFloat_FromDouble(agg.sum);
                break;
            case mimicapi::AggregateKind::kMin:
                if (agg.has_value) {
                    value = PyFloat_FromDouble(agg.min);
                } else {
                    Py_INCREF(Py_None);
                    value = Py_None;
                }
                break;
            case mimicapi::AggregateKind::kMax:
                if (agg.has_value) {
                    value = PyFloat_FromDouble(agg.max);
                } else {
                    Py_INCREF(Py_None);
                    value = Py_None;
                }
                break;
        }
        if (!value) {
            Py_DECREF(dict);
            return nullptr;
        }
        PyDict_SetItemString(dict, key.c_str(), value);
        Py_DECREF(value);
    }
    PyDict_SetItemString(dict, "rows_scanned",
                         PyLong_FromUnsignedLongLong(result.rows_scanned));
    PyDict_SetItemString(dict, "rows_pruned",
                         PyLong_FromUnsignedLongLong(result.rows_pruned));
    return dict;
}

PyObject* ApiClientCoreCompressionStats(ApiClientCoreObject* self, PyObject* args) {
    const char* db = nullptr;
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "ss", &db, &name)) {
        return nullptr;
    }
    std::string error;
    const auto stats = self->core->CompressionStatsFor(db, name, &error);
    if (!error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return nullptr;
    }
    PyObject* dict = PyDict_New();
    PyDict_SetItemString(dict, "raw_bytes",
                         PyLong_FromUnsignedLongLong(stats.raw_bytes));
    PyDict_SetItemString(dict, "compressed_bytes",
                         PyLong_FromUnsignedLongLong(stats.compressed_bytes));
    PyDict_SetItemString(dict, "segments",
                         PyLong_FromSize_t(stats.segments));
    PyDict_SetItemString(dict, "compressed_segments",
                         PyLong_FromSize_t(stats.compressed_segments));
    PyDict_SetItemString(dict, "compressed_columns",
                         PyLong_FromSize_t(stats.compressed_columns));
    PyDict_SetItemString(dict, "active_rows",
                         PyLong_FromSize_t(stats.active_rows));
    return dict;
}

PyObject* SetCompressionEnabled(PyObject*, PyObject* args) {
    int enabled = 0;
    if (!PyArg_ParseTuple(args, "p", &enabled)) {
        return nullptr;
    }
    mimicdb::CompressionConfig cfg = mimicdb::GetCompressionConfig();
    cfg.enabled = enabled != 0;
    mimicdb::SetCompressionConfig(cfg);
    Py_RETURN_NONE;
}

PyMethodDef ApiClientCoreMethods[] = {
    {"create_database", (PyCFunction)ApiClientCoreCreateDatabase, METH_VARARGS, nullptr},
    {"create_dataset", (PyCFunction)ApiClientCoreCreateDataset, METH_VARARGS, nullptr},
    {"drop_database", (PyCFunction)ApiClientCoreDropDatabase, METH_VARARGS, nullptr},
    {"drop_dataset", (PyCFunction)ApiClientCoreDropDataset, METH_VARARGS, nullptr},
    {"append_batch", (PyCFunction)ApiClientCoreAppendBatch, METH_VARARGS, nullptr},
    {"scan", (PyCFunction)ApiClientCoreScan, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"scan_debug", (PyCFunction)ApiClientCoreScanDebug, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"aggregate", (PyCFunction)ApiClientCoreAggregate, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"aggregate_multi", (PyCFunction)ApiClientCoreAggregateMulti,
     METH_VARARGS | METH_KEYWORDS, nullptr},
    {"compression_stats", (PyCFunction)ApiClientCoreCompressionStats, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

PyTypeObject ApiClientCoreType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
};

PyMethodDef ModuleMethods[] = {
    {"set_compression_enabled", (PyCFunction)SetCompressionEnabled, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef ModuleDef = {
    PyModuleDef_HEAD_INIT,
    "mimicapi._mimicapi_core",
    nullptr,
    -1,
    ModuleMethods,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

}  // namespace

PyMODINIT_FUNC PyInit__mimicapi_core(void) {
    ApiClientCoreType.tp_name = "mimicapi._mimicapi_core.ApiClientCore";
    ApiClientCoreType.tp_basicsize = sizeof(ApiClientCoreObject);
    ApiClientCoreType.tp_flags = Py_TPFLAGS_DEFAULT;
    ApiClientCoreType.tp_new = PyType_GenericNew;
    ApiClientCoreType.tp_init = (initproc)ApiClientCoreInit;
    ApiClientCoreType.tp_dealloc = (destructor)ApiClientCoreDealloc;
    ApiClientCoreType.tp_methods = ApiClientCoreMethods;
    if (PyType_Ready(&ApiClientCoreType) < 0) {
        return nullptr;
    }
    PyObject* module = PyModule_Create(&ModuleDef);
    if (!module) {
        return nullptr;
    }
    Py_INCREF(&ApiClientCoreType);
    PyModule_AddObject(module, "ApiClientCore", reinterpret_cast<PyObject*>(&ApiClientCoreType));
    return module;
}
