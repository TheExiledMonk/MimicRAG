#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "mimicdb/aggregate.h"
#include "mimicdb/dataset.h"
#include "mimicdb/field_vector.h"
#include "mimicdb/mask.h"
#include "mimicdb/predicate.h"
#include "mimicdb/scan.h"
#include "mimicdb/segment.h"
#include "mimicdb/types.h"
#include "mimicdb/compression.h"

namespace {

struct DatasetObject {
    PyObject_HEAD
    mimicdb::Dataset* dataset = nullptr;
    std::vector<std::string>* field_names = nullptr;
    std::vector<mimicdb::FieldType>* field_types = nullptr;
    std::unordered_map<std::string, size_t>* field_index = nullptr;
};

struct ParsedPredicate {
    size_t field_index = 0;
    mimicdb::CompareOp op = mimicdb::CompareOp::kEq;
    double value = 0.0;
    mimicdb::FieldType field_type = mimicdb::FieldType::kInt32;
};

struct DecodedColumns {
    std::vector<mimicdb::CompressedColumnView> views;
    std::vector<std::vector<uint8_t>> data;
    std::vector<std::vector<uint8_t>> aux;
};

bool NeedsLz4Decode(const std::vector<mimicdb::CompressedColumnView>& columns) {
    for (const auto& col : columns) {
        if (col.kind == mimicdb::ColumnCompressionKind::kLz4) {
            return true;
        }
    }
    return false;
}

bool BuildReadableColumns(const std::vector<mimicdb::CompressedColumnView>& columns,
                          DecodedColumns* out) {
    if (!out) {
        return false;
    }
    out->views.clear();
    out->views.reserve(columns.size());
    if (out->data.size() < columns.size()) {
        out->data.resize(columns.size());
    }
    if (out->aux.size() < columns.size()) {
        out->aux.resize(columns.size());
    }
    for (size_t i = 0; i < columns.size(); ++i) {
        out->data[i].clear();
        out->aux[i].clear();
        const auto& col = columns[i];
        if (col.kind == mimicdb::ColumnCompressionKind::kLz4) {
            if (col.raw_data_size > 0) {
                out->data[i].resize(col.raw_data_size);
                if (!mimicdb::DecodeLz4Literal(col.data, col.data_size,
                                               out->data[i].data(),
                                               out->data[i].size())) {
                    return false;
                }
            }
            if (col.raw_aux_size > 0) {
                out->aux[i].resize(col.raw_aux_size);
                if (!mimicdb::DecodeLz4Literal(col.aux, col.aux_size,
                                               out->aux[i].data(),
                                               out->aux[i].size())) {
                    return false;
                }
            }
            mimicdb::CompressedColumnView view = col;
            view.kind = mimicdb::ColumnCompressionKind::kNone;
            view.data = out->data[i].empty() ? nullptr : out->data[i].data();
            view.data_size = col.raw_data_size;
            view.aux = out->aux[i].empty() ? nullptr : out->aux[i].data();
            view.aux_size = col.raw_aux_size;
            out->views.push_back(view);
        } else {
            out->views.push_back(col);
        }
    }
    return true;
}

bool BuildMaskCompressedColumns(const std::vector<mimicdb::CompressedColumnView>& columns,
                                const std::vector<ParsedPredicate>& predicates,
                                mimicdb::Mask* out_mask) {
    if (!out_mask) {
        return false;
    }
    if (predicates.empty()) {
        out_mask->Resize(0);
        return false;
    }
    bool has_mask = false;
    mimicdb::Mask mask;
    for (const auto& pred : predicates) {
        if (pred.field_index >= columns.size()) {
            return false;
        }
        const auto& col = columns[pred.field_index];
        mimicdb::CompressionPredicate cpred;
        cpred.type = col.type;
        cpred.op = pred.op;
        cpred.i64 = static_cast<int64_t>(pred.value);
        cpred.f64 = pred.value;
        mimicdb::Mask current;
        if (col.ops && col.ops->scan_predicate) {
            mimicdb::BuildMaskCompressed(col, cpred, &current);
        } else {
            current.Resize(0);
            return false;
        }
        if (!has_mask) {
            mask = std::move(current);
            has_mask = true;
        } else {
            mask = mimicdb::Mask::And(mask, current);
        }
    }
    *out_mask = std::move(mask);
    return has_mask;
}

bool ReadNumericField(const mimicdb::FieldVector& field, size_t index, double* out) {
    if (field.HasNulls() && !field.IsValid(index)) {
        return false;
    }
    switch (field.Type()) {
        case mimicdb::FieldType::kInt32:
            *out = static_cast<double>(field.DataInt32()[index]);
            return true;
        case mimicdb::FieldType::kInt64:
            *out = static_cast<double>(field.DataInt64()[index]);
            return true;
        case mimicdb::FieldType::kFloat64:
            *out = field.DataFloat64()[index];
            return true;
        case mimicdb::FieldType::kBool:
            *out = field.DataBool()[index] ? 1.0 : 0.0;
            return true;
        case mimicdb::FieldType::kDictInt32:
            *out = static_cast<double>(field.DictionaryValue(field.DataDictIds()[index]));
            return true;
        default:
            return false;
    }
}

bool MatchSingleEq(const mimicdb::FieldVector& field, const ParsedPredicate& pred,
                   size_t index) {
    if (field.HasNulls() && !field.IsValid(index)) {
        return false;
    }
    switch (pred.field_type) {
        case mimicdb::FieldType::kInt32:
            return field.DataInt32()[index] == static_cast<int32_t>(pred.value);
        case mimicdb::FieldType::kInt64:
            return field.DataInt64()[index] == static_cast<int64_t>(pred.value);
        case mimicdb::FieldType::kFloat64:
            return field.DataFloat64()[index] == pred.value;
        case mimicdb::FieldType::kBool:
            return (field.DataBool()[index] != 0) == (pred.value != 0.0);
        case mimicdb::FieldType::kDictInt32:
            return field.DictionaryValue(field.DataDictIds()[index]) ==
                   static_cast<int64_t>(pred.value);
        default:
            return false;
    }
}

bool MatchSingleEqCompressed(const mimicdb::CompressedColumnView& column,
                             const ParsedPredicate& pred, size_t index) {
    if (!mimicdb::IsValid(column, index)) {
        return false;
    }
    double value = 0.0;
    if (!mimicdb::ReadNumericValue(column, index, &value)) {
        return false;
    }
    return value == pred.value;
}

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
    if (type_name == "dict_int32") {
        return mimicdb::FieldType::kDictInt32;
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
    if (type_name == "int16") {
        return mimicdb::FieldType::kInt32;
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

int DatasetInit(DatasetObject* self, PyObject* args, PyObject* kwargs) {
    const char* name = nullptr;
    PyObject* fields_obj = nullptr;
    static const char* kwlist[] = {"name", "fields", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sO", const_cast<char**>(kwlist),
                                     &name, &fields_obj)) {
        return -1;
    }
    if (!PyDict_Check(fields_obj)) {
        PyErr_SetString(PyExc_TypeError, "fields must be a dict");
        return -1;
    }
    self->dataset = new mimicdb::Dataset(name);
    self->field_names = new std::vector<std::string>();
    self->field_types = new std::vector<mimicdb::FieldType>();
    self->field_index = new std::unordered_map<std::string, size_t>();
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(fields_obj, &pos, &key, &value)) {
        if (!PyUnicode_Check(key) || !PyUnicode_Check(value)) {
            PyErr_SetString(PyExc_TypeError, "field names and types must be strings");
            return -1;
        }
        PyObject* key_bytes = PyUnicode_AsUTF8String(key);
        PyObject* value_bytes = PyUnicode_AsUTF8String(value);
        if (!key_bytes || !value_bytes) {
            Py_XDECREF(key_bytes);
            Py_XDECREF(value_bytes);
            return -1;
        }
        std::string field_name = PyBytes_AsString(key_bytes);
        std::string type_name = PyBytes_AsString(value_bytes);
        Py_DECREF(key_bytes);
        Py_DECREF(value_bytes);

        bool ok = false;
        const auto type = ParseFieldType(type_name, &ok);
        if (!ok) {
            PyErr_SetString(PyExc_ValueError, "unsupported field type");
            return -1;
        }
        (*self->field_index)[field_name] = self->field_names->size();
        self->field_names->push_back(field_name);
        self->field_types->push_back(type);
        self->dataset->AddField(mimicdb::FieldVector(field_name, type));
    }
    return 0;
}

void DatasetDealloc(DatasetObject* self) {
    delete self->dataset;
    self->dataset = nullptr;
    delete self->field_names;
    delete self->field_types;
    delete self->field_index;
    self->field_names = nullptr;
    self->field_types = nullptr;
    self->field_index = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

PyObject* DatasetAppend(DatasetObject* self, PyObject* args, PyObject* kwargs) {
    if (!kwargs) {
        PyErr_SetString(PyExc_ValueError, "append requires keyword args");
        return nullptr;
    }
    std::vector<mimicdb::FieldValue> values;
    values.reserve(self->field_names->size());
    for (size_t i = 0; i < self->field_names->size(); ++i) {
        const auto& name = (*self->field_names)[i];
        PyObject* value = PyDict_GetItemString(kwargs, name.c_str());
        if (!value) {
            PyErr_SetString(PyExc_ValueError, "append requires values for all fields");
            return nullptr;
        }
        const auto type = (*self->field_types)[i];
        if (value == Py_None) {
            values.push_back(mimicdb::FieldValue::Null(type));
            continue;
        }
        if (type == mimicdb::FieldType::kBool) {
            const int truth = PyObject_IsTrue(value);
            if (truth < 0) {
                return nullptr;
            }
            values.push_back(mimicdb::FieldValue::Bool(truth != 0));
            continue;
        }
        if (type == mimicdb::FieldType::kString) {
            Py_ssize_t size = 0;
            const char* data = PyUnicode_AsUTF8AndSize(value, &size);
            if (!data) {
                return nullptr;
            }
            values.push_back(mimicdb::FieldValue::String(std::string(data, size)));
            continue;
        }
        if (type == mimicdb::FieldType::kBytes) {
            if (!PyBytes_Check(value)) {
                PyErr_SetString(PyExc_TypeError, "bytes field requires bytes");
                return nullptr;
            }
            Py_ssize_t size = 0;
            char* data = nullptr;
            if (PyBytes_AsStringAndSize(value, &data, &size) != 0) {
                return nullptr;
            }
            values.push_back(mimicdb::FieldValue::Bytes(std::string(data, size)));
            continue;
        }
        if (type == mimicdb::FieldType::kFloat64) {
            const double val = PyFloat_AsDouble(value);
            if (PyErr_Occurred()) {
                return nullptr;
            }
            values.push_back(mimicdb::FieldValue::Float64(val));
            continue;
        }
        const long long val = PyLong_AsLongLong(value);
        if (PyErr_Occurred()) {
            return nullptr;
        }
        if (type == mimicdb::FieldType::kInt32 || type == mimicdb::FieldType::kDictInt32) {
            values.push_back(mimicdb::FieldValue::Int32(static_cast<int32_t>(val)));
        } else {
            values.push_back(mimicdb::FieldValue::Int64(static_cast<int64_t>(val)));
        }
    }
    if (!self->dataset->Append(values)) {
        PyErr_SetString(PyExc_RuntimeError, "append failed");
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* DatasetAppendBatch(DatasetObject* self, PyObject* args) {
    PyObject* columns_obj = nullptr;
    if (!PyArg_ParseTuple(args, "O", &columns_obj)) {
        return nullptr;
    }
    if (!PyDict_Check(columns_obj)) {
        PyErr_SetString(PyExc_TypeError, "append_batch requires a dict");
        return nullptr;
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
    i32_values.reserve(self->field_names->size());
    i64_values.reserve(self->field_names->size());
    f64_values.reserve(self->field_names->size());
    bool_values.reserve(self->field_names->size());
    length_values.reserve(self->field_names->size());
    bytes_values.reserve(self->field_names->size());
    validity_buffers.reserve(self->field_names->size());

    std::vector<mimicdb::FieldBatch> batches;
    batches.reserve(self->field_names->size());

    for (size_t i = 0; i < self->field_names->size(); ++i) {
        const auto& name = (*self->field_names)[i];
        PyObject* column = PyDict_GetItemString(columns_obj, name.c_str());
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
        batch.type = (*self->field_types)[i];
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
                const int truth = PyObject_IsTrue(item);
                if (truth < 0) {
                    Py_DECREF(seq);
                    return nullptr;
                }
                values.push_back(truth ? 1U : 0U);
                validity.push_back(1);
            }
            bool_values.push_back(std::move(values));
            batch.data = bool_values.back().data();
        } else if (batch.type == mimicdb::FieldType::kString ||
                   batch.type == mimicdb::FieldType::kBytes) {
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
                if (batch.type == mimicdb::FieldType::kString) {
                    Py_ssize_t size = 0;
                    const char* data = PyUnicode_AsUTF8AndSize(item, &size);
                    if (!data) {
                        Py_DECREF(seq);
                        return nullptr;
                    }
                    lengths.push_back(static_cast<uint32_t>(size));
                    bytes.insert(bytes.end(),
                                 reinterpret_cast<const uint8_t*>(data),
                                 reinterpret_cast<const uint8_t*>(data) + size);
                    validity.push_back(1);
                } else {
                    if (!PyBytes_Check(item)) {
                        Py_DECREF(seq);
                        PyErr_SetString(PyExc_TypeError, "bytes field requires bytes");
                        return nullptr;
                    }
                    Py_ssize_t size = 0;
                    char* data = nullptr;
                    if (PyBytes_AsStringAndSize(item, &data, &size) != 0) {
                        Py_DECREF(seq);
                        return nullptr;
                    }
                    lengths.push_back(static_cast<uint32_t>(size));
                    bytes.insert(bytes.end(),
                                 reinterpret_cast<const uint8_t*>(data),
                                 reinterpret_cast<const uint8_t*>(data) + size);
                    validity.push_back(1);
                }
            }
            if (bytes.empty()) {
                bytes.push_back(0);
            }
            length_values.push_back(std::move(lengths));
            bytes_values.push_back(std::move(bytes));
            batch.lengths = length_values.back().data();
            batch.bytes = bytes_values.back().data();
            batch.bytes_size = bytes_values.back().size();
            batch.data = nullptr;
        } else {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_ValueError, "append_batch unsupported field type");
            return nullptr;
        }
        Py_DECREF(seq);

        if (validity.empty()) {
            batch.validity = nullptr;
        } else {
            validity_buffers.push_back(std::move(validity));
            batch.validity = validity_buffers.back().data();
        }
        batches.push_back(batch);
    }

    if (!self->dataset->AppendBatch(batches)) {
        PyErr_SetString(PyExc_RuntimeError, "append_batch failed");
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* DatasetAggregateInternal(DatasetObject* self, PyObject* predicates_obj, PyObject* agg_obj,
                                   bool debug) {
    if (!predicates_obj || !agg_obj) {
        PyErr_SetString(PyExc_ValueError, "aggregate requires predicates and spec");
        return nullptr;
    }
    if (!PyList_Check(predicates_obj)) {
        PyErr_SetString(PyExc_TypeError, "predicates must be a list");
        return nullptr;
    }
    if (!PyDict_Check(agg_obj)) {
        PyErr_SetString(PyExc_TypeError, "aggregate spec must be a dict");
        return nullptr;
    }

    std::vector<ParsedPredicate> predicates;
    const Py_ssize_t pred_count = PyList_Size(predicates_obj);
    predicates.reserve(static_cast<size_t>(pred_count));
    for (Py_ssize_t i = 0; i < pred_count; ++i) {
        PyObject* pred = PyList_GetItem(predicates_obj, i);
        if (!PyTuple_Check(pred) || PyTuple_Size(pred) != 3) {
            PyErr_SetString(PyExc_TypeError, "predicate must be tuple(field, op, value)");
            return nullptr;
        }
        PyObject* field_obj = PyTuple_GetItem(pred, 0);
        PyObject* op_obj = PyTuple_GetItem(pred, 1);
        PyObject* value_obj = PyTuple_GetItem(pred, 2);
        if (!PyUnicode_Check(field_obj) || !PyUnicode_Check(op_obj)) {
            PyErr_SetString(PyExc_TypeError, "predicate field/op must be str");
            return nullptr;
        }
        PyObject* field_bytes = PyUnicode_AsUTF8String(field_obj);
        PyObject* op_bytes = PyUnicode_AsUTF8String(op_obj);
        if (!field_bytes || !op_bytes) {
            Py_XDECREF(field_bytes);
            Py_XDECREF(op_bytes);
            return nullptr;
        }
        std::string field_name = PyBytes_AsString(field_bytes);
        std::string op_name = PyBytes_AsString(op_bytes);
        Py_DECREF(field_bytes);
        Py_DECREF(op_bytes);
        auto it = self->field_index->find(field_name);
        if (it == self->field_index->end()) {
            PyErr_SetString(PyExc_ValueError, "unknown field in predicate");
            return nullptr;
        }
        mimicdb::CompareOp op;
        if (!ParseCompareOp(op_name, &op)) {
            PyErr_SetString(PyExc_ValueError, "unsupported compare op");
            return nullptr;
        }
        const auto& field = self->dataset->Fields()[it->second];
        const auto type = field.Type();
        double value = 0.0;
        if (type == mimicdb::FieldType::kString || type == mimicdb::FieldType::kBytes) {
            PyErr_SetString(PyExc_TypeError, "string/bytes predicates not supported in C++ backend");
            return nullptr;
        }
        if (type == mimicdb::FieldType::kBool) {
            const int truth = PyObject_IsTrue(value_obj);
            if (truth < 0) {
                return nullptr;
            }
            value = truth ? 1.0 : 0.0;
        } else if (type == mimicdb::FieldType::kFloat64) {
            value = PyFloat_AsDouble(value_obj);
            if (PyErr_Occurred()) {
                return nullptr;
            }
        } else {
            const long long val = PyLong_AsLongLong(value_obj);
            if (PyErr_Occurred()) {
                return nullptr;
            }
            value = static_cast<double>(val);
        }
        ParsedPredicate parsed;
        parsed.field_index = it->second;
        parsed.op = op;
        parsed.value = value;
        parsed.field_type = type;
        predicates.push_back(parsed);
    }
    std::vector<const mimicdb::Segment*> segments;
    segments.reserve(self->dataset->Segments().size() + 1);
    for (const auto& seg : self->dataset->Segments()) {
        segments.push_back(&seg);
    }
    mimicdb::Segment active_segment(0, 0, {});
    if (self->dataset->ActiveRowCount() > 0) {
        active_segment = mimicdb::Segment(self->dataset->SegmentCapacity(),
                                       self->dataset->ActiveRowCount(),
                                       self->dataset->ActiveFields());
        segments.push_back(&active_segment);
    }

    mimicdb::Metrics metrics;
    if (debug) {
        metrics.AddSegmentsTotal(static_cast<uint64_t>(segments.size()));
    }
    uint64_t rows_scanned = 0;
    uint64_t rows_pruned = 0;

    PyObject* result = PyDict_New();
    if (!result) {
        return nullptr;
    }

    size_t sum_index = 0;
    size_t min_index = 0;
    size_t max_index = 0;
    auto get_field_index = [&](PyObject* field_obj, size_t* index_out) -> bool {
        if (!field_obj || field_obj == Py_None) {
            return false;
        }
        if (!PyUnicode_Check(field_obj)) {
            PyErr_SetString(PyExc_TypeError, "aggregate field must be str");
            return false;
        }
        PyObject* field_bytes = PyUnicode_AsUTF8String(field_obj);
        if (!field_bytes) {
            return false;
        }
        std::string field_name = PyBytes_AsString(field_bytes);
        Py_DECREF(field_bytes);
        auto it = self->field_index->find(field_name);
        if (it == self->field_index->end()) {
            PyErr_SetString(PyExc_ValueError, "unknown field in aggregate");
            return false;
        }
        *index_out = it->second;
        return true;
    };

    PyObject* sum_field = PyDict_GetItemString(agg_obj, "sum");
    PyObject* min_field = PyDict_GetItemString(agg_obj, "min");
    PyObject* max_field = PyDict_GetItemString(agg_obj, "max");
    PyObject* count_flag = PyDict_GetItemString(agg_obj, "count");

    const bool has_sum = get_field_index(sum_field, &sum_index);
    const bool has_min = get_field_index(min_field, &min_index);
    const bool has_max = get_field_index(max_field, &max_index);
    const bool sum_matches_min = !has_min || sum_index == min_index;
    const bool sum_matches_max = !has_max || sum_index == max_index;
    const bool need_mixed = has_sum && (has_min || has_max) && sum_matches_min && sum_matches_max;
    const bool single_eq =
        predicates.size() == 1 && predicates[0].op == mimicdb::CompareOp::kEq;

    auto check_agg_type = [&](size_t index) -> bool {
        const auto type = self->dataset->Fields()[index].Type();
        return type != mimicdb::FieldType::kString && type != mimicdb::FieldType::kBytes;
    };
    if ((has_sum && !check_agg_type(sum_index)) ||
        (has_min && !check_agg_type(min_index)) ||
        (has_max && !check_agg_type(max_index))) {
        PyErr_SetString(PyExc_TypeError, "aggregate not supported for string/bytes fields");
        return nullptr;
    }

    mimicdb::AggregateResult sum_acc;
    mimicdb::AggregateResult minmax_acc;
    mimicdb::AggregateResult mixed_acc;
    bool mixed_has_value = false;
    uint64_t match_count = 0;

    auto count_mask_bits = [](const mimicdb::Mask& mask) -> uint64_t {
        uint64_t count = 0;
        const size_t size = mask.Size();
        for (size_t i = 0; i < size; ++i) {
            if (mask.Get(i)) {
                count += 1;
            }
        }
        return count;
    };

    for (const auto* seg : segments) {
        const size_t count = seg->RowCount();
        bool matches = true;
        for (const auto& pred : predicates) {
            const auto& stats = seg->ColumnStats();
            if (pred.field_index >= stats.size() ||
                !mimicdb::SegmentMatchesPredicate(stats[pred.field_index], pred.op, pred.value)) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            if (debug) {
                metrics.AddSegmentsPruned(1);
            }
            rows_pruned += count;
            continue;
        }
        if (debug) {
            metrics.AddSegmentsScanned(1);
        }
        rows_scanned += count;
        const bool compressed = seg->IsSealed() && !seg->CompressedColumns().empty();
        if (compressed) {
            const auto& cols = seg->CompressedColumns();
            const auto* working = &cols;
            if (NeedsLz4Decode(cols)) {
                static thread_local DecodedColumns decoded;
                if (!BuildReadableColumns(cols, &decoded)) {
                    PyErr_SetString(PyExc_RuntimeError, "lz4 decode failed");
                    return nullptr;
                }
                working = &decoded.views;
            }
            auto update_agg = [](mimicdb::AggregateResult* acc, double value) {
                acc->count += 1;
                acc->sum += value;
                if (!acc->has_value) {
                    acc->min = value;
                    acc->max = value;
                    acc->has_value = true;
                } else {
                    acc->min = std::min(acc->min, value);
                    acc->max = std::max(acc->max, value);
                }
            };
            if (single_eq && predicates[0].field_index < working->size()) {
                const auto& pred = predicates[0];
                const auto& pred_col = (*working)[pred.field_index];
                for (size_t row = 0; row < count; ++row) {
                    if (!MatchSingleEqCompressed(pred_col, pred, row)) {
                        continue;
                    }
                    match_count += 1;
                    if (need_mixed) {
                        double value = 0.0;
                        if (!mimicdb::ReadNumericValue((*working)[sum_index], row, &value)) {
                            continue;
                        }
                        update_agg(&mixed_acc, value);
                        mixed_has_value = true;
                    } else {
                        if (has_sum) {
                            double value = 0.0;
                            if (mimicdb::ReadNumericValue((*working)[sum_index], row, &value)) {
                                sum_acc.sum += value;
                                sum_acc.count += 1;
                            }
                        }
                        if (has_min || has_max) {
                            const size_t idx = has_min ? min_index : max_index;
                            double value = 0.0;
                            if (mimicdb::ReadNumericValue((*working)[idx], row, &value)) {
                                update_agg(&minmax_acc, value);
                            }
                        }
                    }
                }
            } else {
                mimicdb::Mask mask;
                const bool has_mask =
                    BuildMaskCompressedColumns(*working, predicates, &mask);
                const mimicdb::Mask* mask_ptr = has_mask ? &mask : nullptr;
                if (has_mask) {
                    match_count += count_mask_bits(mask);
                } else {
                    match_count += count;
                }
                if (need_mixed) {
                    mimicdb::AggregateResult seg_result;
                    mimicdb::AggregateCompressed((*working)[sum_index], mask_ptr, &seg_result);
                    mixed_acc.sum += seg_result.sum;
                    mixed_acc.count += seg_result.count;
                    if (seg_result.has_value) {
                        if (!mixed_has_value) {
                            mixed_acc.min = seg_result.min;
                            mixed_acc.max = seg_result.max;
                            mixed_acc.has_value = true;
                            mixed_has_value = true;
                        } else {
                            if (seg_result.min < mixed_acc.min) {
                                mixed_acc.min = seg_result.min;
                            }
                            if (seg_result.max > mixed_acc.max) {
                                mixed_acc.max = seg_result.max;
                            }
                        }
                    }
                } else {
                    if (has_sum) {
                        mimicdb::AggregateResult seg_sum;
                        mimicdb::AggregateCompressed((*working)[sum_index], mask_ptr, &seg_sum);
                        sum_acc.sum += seg_sum.sum;
                        sum_acc.count += seg_sum.count;
                    }
                    if (has_min || has_max) {
                        const size_t idx = has_min ? min_index : max_index;
                        mimicdb::AggregateResult seg_mm;
                        mimicdb::AggregateCompressed((*working)[idx], mask_ptr, &seg_mm);
                        if (seg_mm.has_value) {
                            if (!minmax_acc.has_value) {
                                minmax_acc = seg_mm;
                            } else {
                                if (seg_mm.min < minmax_acc.min) {
                                    minmax_acc.min = seg_mm.min;
                                }
                                if (seg_mm.max > minmax_acc.max) {
                                    minmax_acc.max = seg_mm.max;
                                }
                                minmax_acc.count += seg_mm.count;
                            }
                        } else {
                            minmax_acc.count += seg_mm.count;
                        }
                    }
                }
            }
            continue;
        }
        if (single_eq) {
            const auto& pred = predicates[0];
            const auto& pred_field = seg->Fields()[pred.field_index];
            for (size_t row = 0; row < count; ++row) {
                if (!MatchSingleEq(pred_field, pred, row)) {
                    continue;
                }
                match_count += 1;
                if (need_mixed) {
                    double value = 0.0;
                    if (!ReadNumericField(seg->Fields()[sum_index], row, &value)) {
                        continue;
                    }
                    mixed_acc.count += 1;
                    mixed_acc.sum += value;
                    if (!mixed_acc.has_value) {
                        mixed_acc.min = value;
                        mixed_acc.max = value;
                        mixed_acc.has_value = true;
                        mixed_has_value = true;
                    } else {
                        mixed_acc.min = std::min(mixed_acc.min, value);
                        mixed_acc.max = std::max(mixed_acc.max, value);
                    }
                } else {
                    if (has_sum) {
                        double value = 0.0;
                        if (ReadNumericField(seg->Fields()[sum_index], row, &value)) {
                            sum_acc.sum += value;
                            sum_acc.count += 1;
                        }
                    }
                    if (has_min || has_max) {
                        const size_t idx = has_min ? min_index : max_index;
                        double value = 0.0;
                        if (ReadNumericField(seg->Fields()[idx], row, &value)) {
                            minmax_acc.count += 1;
                            if (!minmax_acc.has_value) {
                                minmax_acc.min = value;
                                minmax_acc.max = value;
                                minmax_acc.has_value = true;
                            } else {
                                minmax_acc.min = std::min(minmax_acc.min, value);
                                minmax_acc.max = std::max(minmax_acc.max, value);
                            }
                        }
                    }
                }
            }
        } else {
            mimicdb::Mask mask(count);
            for (size_t i = 0; i < count; ++i) {
                mask.Set(i, true);
            }
            for (const auto& pred : predicates) {
                const auto& field = seg->Fields()[pred.field_index];
                const auto type = field.Type();
                for (size_t row = 0; row < count; ++row) {
                    if (!mask.Get(row)) {
                        continue;
                    }
                    if (field.HasNulls() && !field.IsValid(row)) {
                        mask.Set(row, false);
                        continue;
                    }
                    bool keep = false;
                    switch (type) {
                        case mimicdb::FieldType::kInt32:
                            keep = mimicdb::CompareInt64(field.DataInt32()[row],
                                                      static_cast<int64_t>(pred.value), pred.op);
                            break;
                        case mimicdb::FieldType::kInt64:
                            keep = mimicdb::CompareInt64(field.DataInt64()[row],
                                                      static_cast<int64_t>(pred.value), pred.op);
                            break;
                        case mimicdb::FieldType::kFloat64:
                            keep = mimicdb::CompareFloat64(field.DataFloat64()[row], pred.value,
                                                           pred.op);
                            break;
                        case mimicdb::FieldType::kBool:
                            keep = mimicdb::CompareInt64(field.DataBool()[row] ? 1 : 0,
                                                      static_cast<int64_t>(pred.value), pred.op);
                            break;
                        case mimicdb::FieldType::kDictInt32: {
                            const auto* dict = field.Dictionary();
                            const auto* ids = field.DataDictIds();
                            keep = mimicdb::CompareInt64(dict->Value(ids[row]),
                                                      static_cast<int64_t>(pred.value), pred.op);
                            break;
                        }
                        default:
                            break;
                    }
                    if (!keep) {
                        mask.Set(row, false);
                    }
                }
            }
            match_count += count_mask_bits(mask);

            if (need_mixed) {
                mimicdb::AggregateResult seg_result;
                mimicdb::AggregateMixed(seg->Fields()[sum_index], &mask, &seg_result);
                mixed_acc.sum += seg_result.sum;
                mixed_acc.count += seg_result.count;
                if (seg_result.has_value) {
                    if (!mixed_has_value) {
                        mixed_acc.min = seg_result.min;
                        mixed_acc.max = seg_result.max;
                        mixed_acc.has_value = true;
                        mixed_has_value = true;
                    } else {
                        if (seg_result.min < mixed_acc.min) {
                            mixed_acc.min = seg_result.min;
                        }
                        if (seg_result.max > mixed_acc.max) {
                            mixed_acc.max = seg_result.max;
                        }
                    }
                }
            } else {
                if (has_sum) {
                    mimicdb::AggregateResult seg_sum;
                    mimicdb::AggregateSum(seg->Fields()[sum_index], &mask, &seg_sum);
                    sum_acc.sum += seg_sum.sum;
                    sum_acc.count += seg_sum.count;
                }
                if (has_min || has_max) {
                    const size_t idx = has_min ? min_index : max_index;
                    mimicdb::AggregateResult seg_mm;
                    mimicdb::AggregateMinMax(seg->Fields()[idx], &mask, &seg_mm);
                    if (seg_mm.has_value) {
                        if (!minmax_acc.has_value) {
                            minmax_acc = seg_mm;
                        } else {
                            if (seg_mm.min < minmax_acc.min) {
                                minmax_acc.min = seg_mm.min;
                            }
                            if (seg_mm.max > minmax_acc.max) {
                                minmax_acc.max = seg_mm.max;
                            }
                            minmax_acc.count += seg_mm.count;
                        }
                    } else {
                        minmax_acc.count += seg_mm.count;
                    }
                }
            }
        }
    }

    if (has_sum) {
        if (need_mixed) {
            PyDict_SetItemString(result, "sum", PyFloat_FromDouble(mixed_acc.sum));
        } else {
            PyDict_SetItemString(result, "sum", PyFloat_FromDouble(sum_acc.sum));
        }
    }
    if (has_min || has_max) {
        if (need_mixed) {
            if (has_min) {
                PyDict_SetItemString(result, "min",
                                     mixed_acc.has_value ? PyFloat_FromDouble(mixed_acc.min)
                                                         : Py_None);
            }
            if (has_max) {
                PyDict_SetItemString(result, "max",
                                     mixed_acc.has_value ? PyFloat_FromDouble(mixed_acc.max)
                                                         : Py_None);
            }
        } else {
            if (has_min) {
                PyDict_SetItemString(result, "min",
                                     minmax_acc.has_value ? PyFloat_FromDouble(minmax_acc.min)
                                                          : Py_None);
            }
            if (has_max) {
                PyDict_SetItemString(result, "max",
                                     minmax_acc.has_value ? PyFloat_FromDouble(minmax_acc.max)
                                                          : Py_None);
            }
        }
    }

    if (count_flag && PyObject_IsTrue(count_flag)) {
        uint64_t count_value = match_count;
        if (!has_sum && (has_min || has_max)) {
            count_value = minmax_acc.count;
        }
        PyDict_SetItemString(result, "count",
                             PyLong_FromUnsignedLongLong(count_value));
    }

    if (debug) {
        PyObject* stats = PyDict_New();
        PyDict_SetItemString(stats, "segments_total",
                             PyLong_FromUnsignedLongLong(metrics.segments_total));
        PyDict_SetItemString(stats, "segments_scanned",
                             PyLong_FromUnsignedLongLong(metrics.segments_scanned));
        PyDict_SetItemString(stats, "segments_pruned",
                             PyLong_FromUnsignedLongLong(metrics.segments_pruned));
        PyDict_SetItemString(stats, "rows_scanned",
                             PyLong_FromUnsignedLongLong(rows_scanned));
        PyDict_SetItemString(stats, "rows_pruned",
                             PyLong_FromUnsignedLongLong(rows_pruned));
        return PyTuple_Pack(2, result, stats);
    }

    return result;

}

PyObject* DatasetAggregate(DatasetObject* self, PyObject* args) {
    PyObject* predicates_obj = nullptr;
    PyObject* agg_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &predicates_obj, &agg_obj)) {
        return nullptr;
    }
    return DatasetAggregateInternal(self, predicates_obj, agg_obj, false);
}

PyObject* DatasetAggregateDebug(DatasetObject* self, PyObject* args) {
    PyObject* predicates_obj = nullptr;
    PyObject* agg_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &predicates_obj, &agg_obj)) {
        return nullptr;
    }
    return DatasetAggregateInternal(self, predicates_obj, agg_obj, true);
}

PyObject* DatasetScan(DatasetObject* self, PyObject* args) {
    PyObject* predicates_obj = nullptr;
    PyObject* columns_obj = nullptr;
    unsigned long long limit = 0;
    unsigned long long offset = 0;
    if (!PyArg_ParseTuple(args, "OO|KK", &predicates_obj, &columns_obj, &limit, &offset)) {
        return nullptr;
    }
    if (!PyList_Check(predicates_obj)) {
        PyErr_SetString(PyExc_TypeError, "predicates must be a list");
        return nullptr;
    }
    std::vector<size_t> column_indices;
    std::vector<std::string> column_names;
    if (columns_obj == Py_None) {
        column_indices.reserve(self->field_names->size());
        column_names.reserve(self->field_names->size());
        for (size_t i = 0; i < self->field_names->size(); ++i) {
            column_indices.push_back(i);
            column_names.push_back((*self->field_names)[i]);
        }
    } else if (PyList_Check(columns_obj)) {
        const Py_ssize_t col_count = PyList_Size(columns_obj);
        column_indices.reserve(static_cast<size_t>(col_count));
        column_names.reserve(static_cast<size_t>(col_count));
        for (Py_ssize_t i = 0; i < col_count; ++i) {
            PyObject* item = PyList_GetItem(columns_obj, i);
            if (!PyUnicode_Check(item)) {
                PyErr_SetString(PyExc_TypeError, "column names must be strings");
                return nullptr;
            }
            PyObject* bytes = PyUnicode_AsUTF8String(item);
            if (!bytes) {
                return nullptr;
            }
            std::string name = PyBytes_AsString(bytes);
            Py_DECREF(bytes);
            auto it = self->field_index->find(name);
            if (it == self->field_index->end()) {
                PyErr_SetString(PyExc_ValueError, "unknown column name");
                return nullptr;
            }
            column_indices.push_back(it->second);
            column_names.push_back(name);
        }
    } else {
        PyErr_SetString(PyExc_TypeError, "columns must be list or None");
        return nullptr;
    }

    std::vector<ParsedPredicate> predicates;
    const Py_ssize_t pred_count = PyList_Size(predicates_obj);
    predicates.reserve(static_cast<size_t>(pred_count));
    for (Py_ssize_t i = 0; i < pred_count; ++i) {
        PyObject* pred = PyList_GetItem(predicates_obj, i);
        if (!PyTuple_Check(pred) || PyTuple_Size(pred) != 3) {
            PyErr_SetString(PyExc_TypeError, "predicate must be tuple(field, op, value)");
            return nullptr;
        }
        PyObject* field_obj = PyTuple_GetItem(pred, 0);
        PyObject* op_obj = PyTuple_GetItem(pred, 1);
        PyObject* value_obj = PyTuple_GetItem(pred, 2);
        if (!PyUnicode_Check(field_obj) || !PyUnicode_Check(op_obj)) {
            PyErr_SetString(PyExc_TypeError, "predicate field/op must be str");
            return nullptr;
        }
        PyObject* field_bytes = PyUnicode_AsUTF8String(field_obj);
        PyObject* op_bytes = PyUnicode_AsUTF8String(op_obj);
        if (!field_bytes || !op_bytes) {
            Py_XDECREF(field_bytes);
            Py_XDECREF(op_bytes);
            return nullptr;
        }
        std::string field_name = PyBytes_AsString(field_bytes);
        std::string op_name = PyBytes_AsString(op_bytes);
        Py_DECREF(field_bytes);
        Py_DECREF(op_bytes);
        auto it = self->field_index->find(field_name);
        if (it == self->field_index->end()) {
            PyErr_SetString(PyExc_ValueError, "unknown field in predicate");
            return nullptr;
        }
        mimicdb::CompareOp op;
        if (!ParseCompareOp(op_name, &op)) {
            PyErr_SetString(PyExc_ValueError, "unsupported compare op");
            return nullptr;
        }
        const auto& field = self->dataset->Fields()[it->second];
        const auto type = field.Type();
        double value = 0.0;
        if (type == mimicdb::FieldType::kString || type == mimicdb::FieldType::kBytes) {
            PyErr_SetString(PyExc_TypeError, "string/bytes predicates not supported in C++ backend");
            return nullptr;
        }
        if (type == mimicdb::FieldType::kBool) {
            const int truth = PyObject_IsTrue(value_obj);
            if (truth < 0) {
                return nullptr;
            }
            value = truth ? 1.0 : 0.0;
        } else if (type == mimicdb::FieldType::kFloat64) {
            value = PyFloat_AsDouble(value_obj);
            if (PyErr_Occurred()) {
                return nullptr;
            }
        } else {
            const long long val = PyLong_AsLongLong(value_obj);
            if (PyErr_Occurred()) {
                return nullptr;
            }
            value = static_cast<double>(val);
        }
        ParsedPredicate parsed;
        parsed.field_index = it->second;
        parsed.op = op;
        parsed.value = value;
        parsed.field_type = type;
        predicates.push_back(parsed);
    }
    const bool single_eq =
        predicates.size() == 1 && predicates[0].op == mimicdb::CompareOp::kEq;

    std::vector<const mimicdb::Segment*> segments;
    segments.reserve(self->dataset->Segments().size() + 1);
    for (const auto& seg : self->dataset->Segments()) {
        segments.push_back(&seg);
    }
    mimicdb::Segment active_segment(0, 0, {});
    if (self->dataset->ActiveRowCount() > 0) {
        active_segment = mimicdb::Segment(self->dataset->SegmentCapacity(),
                                          self->dataset->ActiveRowCount(),
                                          self->dataset->ActiveFields());
        segments.push_back(&active_segment);
    }

    PyObject* rows = PyList_New(0);
    if (!rows) {
        return nullptr;
    }
    uint64_t skipped = 0;
    uint64_t added = 0;
    bool stop = false;

    for (const auto* seg : segments) {
        const size_t count = seg->RowCount();
        const bool compressed = seg->IsSealed() && !seg->CompressedColumns().empty();
        if (compressed) {
            const auto& cols = seg->CompressedColumns();
            const auto* working = &cols;
            if (NeedsLz4Decode(cols)) {
                static thread_local DecodedColumns decoded;
                if (!BuildReadableColumns(cols, &decoded)) {
                    Py_DECREF(rows);
                    PyErr_SetString(PyExc_RuntimeError, "lz4 decode failed");
                    return nullptr;
                }
                working = &decoded.views;
            }
            mimicdb::Mask mask;
            bool has_mask = false;
            if (!single_eq) {
                has_mask = BuildMaskCompressedColumns(*working, predicates, &mask);
            }
            for (size_t row = 0; row < count; ++row) {
                if (limit && added >= limit) {
                    stop = true;
                    break;
                }
                if (single_eq) {
                    const auto& pred = predicates[0];
                    if (pred.field_index >= working->size() ||
                        !MatchSingleEqCompressed((*working)[pred.field_index], pred, row)) {
                        continue;
                    }
                } else {
                    if (has_mask && !mask.Get(row)) {
                        continue;
                    }
                    if (!has_mask && !predicates.empty()) {
                        continue;
                    }
                }
                if (skipped < offset) {
                    skipped += 1;
                    continue;
                }
                PyObject* row_obj = PyDict_New();
                if (!row_obj) {
                    Py_DECREF(rows);
                    return nullptr;
                }
                for (size_t i = 0; i < column_indices.size(); ++i) {
                    const size_t field_index = column_indices[i];
                    const auto& field = (*working)[field_index];
                    const char* name = column_names[i].c_str();
                    if (!mimicdb::IsValid(field, row)) {
                        Py_INCREF(Py_None);
                        PyDict_SetItemString(row_obj, name, Py_None);
                        continue;
                    }
                    PyObject* value = nullptr;
                    switch (field.type) {
                        case mimicdb::FieldType::kInt32: {
                            int64_t v = 0;
                            if (!mimicdb::ReadInt64Value(field, row, &v)) {
                                value = Py_None;
                                Py_INCREF(Py_None);
                            } else {
                                value = PyLong_FromLong(static_cast<long>(v));
                            }
                            break;
                        }
                        case mimicdb::FieldType::kInt64: {
                            int64_t v = 0;
                            if (!mimicdb::ReadInt64Value(field, row, &v)) {
                                value = Py_None;
                                Py_INCREF(Py_None);
                            } else {
                                value = PyLong_FromLongLong(v);
                            }
                            break;
                        }
                        case mimicdb::FieldType::kFloat64: {
                            double v = 0.0;
                            if (!mimicdb::ReadNumericValue(field, row, &v)) {
                                value = Py_None;
                                Py_INCREF(Py_None);
                            } else {
                                value = PyFloat_FromDouble(v);
                            }
                            break;
                        }
                        case mimicdb::FieldType::kBool: {
                            int64_t v = 0;
                            if (!mimicdb::ReadInt64Value(field, row, &v)) {
                                value = Py_None;
                                Py_INCREF(Py_None);
                            } else {
                                value = PyBool_FromLong(v != 0);
                            }
                            break;
                        }
                        case mimicdb::FieldType::kDictInt32: {
                            int64_t v = 0;
                            if (!mimicdb::ReadInt64Value(field, row, &v)) {
                                value = Py_None;
                                Py_INCREF(Py_None);
                            } else {
                                value = PyLong_FromLong(static_cast<long>(v));
                            }
                            break;
                        }
                        case mimicdb::FieldType::kString: {
                            const auto* lengths =
                                reinterpret_cast<const uint32_t*>(field.aux);
                            const auto* bytes = field.data;
                            if (!lengths || !bytes) {
                                value = Py_None;
                                Py_INCREF(Py_None);
                                break;
                            }
                            uint32_t offset_local = 0;
                            for (size_t j = 0; j < row; ++j) {
                                offset_local += lengths[j];
                            }
                            value = PyUnicode_FromStringAndSize(
                                reinterpret_cast<const char*>(bytes + offset_local),
                                static_cast<Py_ssize_t>(lengths[row]));
                            break;
                        }
                        case mimicdb::FieldType::kBytes: {
                            const auto* lengths =
                                reinterpret_cast<const uint32_t*>(field.aux);
                            const auto* bytes = field.data;
                            if (!lengths || !bytes) {
                                value = Py_None;
                                Py_INCREF(Py_None);
                                break;
                            }
                            uint32_t offset_local = 0;
                            for (size_t j = 0; j < row; ++j) {
                                offset_local += lengths[j];
                            }
                            value = PyBytes_FromStringAndSize(
                                reinterpret_cast<const char*>(bytes + offset_local),
                                static_cast<Py_ssize_t>(lengths[row]));
                            break;
                        }
                        case mimicdb::FieldType::kArray:
                        case mimicdb::FieldType::kObject:
                            value = Py_None;
                            Py_INCREF(Py_None);
                            break;
                    }
                    if (!value) {
                        Py_DECREF(row_obj);
                        Py_DECREF(rows);
                        return nullptr;
                    }
                    PyDict_SetItemString(row_obj, name, value);
                    Py_DECREF(value);
                }
                PyList_Append(rows, row_obj);
                Py_DECREF(row_obj);
                added += 1;
                if (limit && added >= limit) {
                    stop = true;
                    break;
                }
            }
            if (stop) {
                break;
            }
            continue;
        }
        auto append_row = [&](size_t row) -> bool {
            PyObject* row_obj = PyDict_New();
            if (!row_obj) {
                Py_DECREF(rows);
                return false;
            }
            for (size_t i = 0; i < column_indices.size(); ++i) {
                const size_t field_index = column_indices[i];
                const auto& field = seg->Fields()[field_index];
                const char* name = column_names[i].c_str();
                if (field.HasNulls() && !field.IsValid(row)) {
                    Py_INCREF(Py_None);
                    PyDict_SetItemString(row_obj, name, Py_None);
                    continue;
                }
                PyObject* value = nullptr;
                switch (field.Type()) {
                    case mimicdb::FieldType::kInt32:
                        value = PyLong_FromLong(field.DataInt32()[row]);
                        break;
                    case mimicdb::FieldType::kInt64:
                        value = PyLong_FromLongLong(field.DataInt64()[row]);
                        break;
                    case mimicdb::FieldType::kFloat64:
                        value = PyFloat_FromDouble(field.DataFloat64()[row]);
                        break;
                    case mimicdb::FieldType::kBool:
                        value = PyBool_FromLong(field.DataBool()[row] != 0);
                        break;
                    case mimicdb::FieldType::kDictInt32:
                        value = PyLong_FromLong(field.DictionaryValue(field.DataDictIds()[row]));
                        break;
                    case mimicdb::FieldType::kString: {
                        const auto* lengths = field.DataLengths();
                        const auto* bytes = field.DataBytes();
                        size_t offset = 0;
                        for (size_t i = 0; i < row; ++i) {
                            offset += lengths[i];
                        }
                        value = PyUnicode_FromStringAndSize(
                            reinterpret_cast<const char*>(bytes + offset),
                            static_cast<Py_ssize_t>(lengths[row]));
                        break;
                    }
                    case mimicdb::FieldType::kBytes: {
                        const auto* lengths = field.DataLengths();
                        const auto* bytes = field.DataBytes();
                        size_t offset = 0;
                        for (size_t i = 0; i < row; ++i) {
                            offset += lengths[i];
                        }
                        value = PyBytes_FromStringAndSize(
                            reinterpret_cast<const char*>(bytes + offset),
                            static_cast<Py_ssize_t>(lengths[row]));
                        break;
                    }
                    case mimicdb::FieldType::kArray:
                    case mimicdb::FieldType::kObject:
                        Py_INCREF(Py_None);
                        value = Py_None;
                        break;
                }
                if (!value) {
                    Py_DECREF(row_obj);
                    Py_DECREF(rows);
                    return false;
                }
                PyDict_SetItemString(row_obj, name, value);
                Py_DECREF(value);
            }
            PyList_Append(rows, row_obj);
            Py_DECREF(row_obj);
            added += 1;
            if (limit && added >= limit) {
                stop = true;
                return true;
            }
            return true;
        };

        if (single_eq) {
            const auto& pred = predicates[0];
            const auto& field = seg->Fields()[pred.field_index];
            for (size_t row = 0; row < count; ++row) {
                if (!MatchSingleEq(field, pred, row)) {
                    continue;
                }
                if (skipped < offset) {
                    skipped += 1;
                    continue;
                }
                if (!append_row(row)) {
                    return nullptr;
                }
                if (stop) {
                    break;
                }
            }
        } else {
            mimicdb::Mask mask(count);
            for (size_t i = 0; i < count; ++i) {
                mask.Set(i, true);
            }
            for (const auto& pred : predicates) {
                const auto& field = seg->Fields()[pred.field_index];
                const auto type = field.Type();
                for (size_t row = 0; row < count; ++row) {
                    if (!mask.Get(row)) {
                        continue;
                    }
                    if (field.HasNulls() && !field.IsValid(row)) {
                        mask.Set(row, false);
                        continue;
                    }
                    bool keep = false;
                    switch (type) {
                        case mimicdb::FieldType::kInt32:
                            keep = mimicdb::CompareInt64(field.DataInt32()[row],
                                                         static_cast<int64_t>(pred.value),
                                                         pred.op);
                            break;
                        case mimicdb::FieldType::kInt64:
                            keep = mimicdb::CompareInt64(field.DataInt64()[row],
                                                         static_cast<int64_t>(pred.value),
                                                         pred.op);
                            break;
                        case mimicdb::FieldType::kFloat64:
                            keep = mimicdb::CompareFloat64(field.DataFloat64()[row], pred.value,
                                                           pred.op);
                            break;
                        case mimicdb::FieldType::kBool:
                            keep = mimicdb::CompareInt64(field.DataBool()[row] ? 1 : 0,
                                                         static_cast<int64_t>(pred.value),
                                                         pred.op);
                            break;
                        case mimicdb::FieldType::kDictInt32: {
                            const auto* dict = field.Dictionary();
                            const auto* ids = field.DataDictIds();
                            keep = mimicdb::CompareInt64(dict->Value(ids[row]),
                                                         static_cast<int64_t>(pred.value),
                                                         pred.op);
                            break;
                        }
                        default:
                            break;
                    }
                    if (!keep) {
                        mask.Set(row, false);
                    }
                }
            }

            for (size_t row = 0; row < count; ++row) {
                if (!mask.Get(row)) {
                    continue;
                }
                if (skipped < offset) {
                    skipped += 1;
                    continue;
                }
                if (!append_row(row)) {
                    return nullptr;
                }
                if (stop) {
                    break;
                }
            }
        }
        if (stop) {
            break;
        }
    }

    return rows;
}

PyObject* DatasetCompressionStats(DatasetObject* self, PyObject*) {
    const auto stats = self->dataset->CompressionStats();
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

PyMethodDef DatasetMethods[] = {
    {"append", (PyCFunction)DatasetAppend, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"append_batch", (PyCFunction)DatasetAppendBatch, METH_VARARGS, nullptr},
    {"aggregate", (PyCFunction)DatasetAggregate, METH_VARARGS, nullptr},
    {"aggregate_debug", (PyCFunction)DatasetAggregateDebug, METH_VARARGS, nullptr},
    {"scan", (PyCFunction)DatasetScan, METH_VARARGS, nullptr},
    {"compression_stats", (PyCFunction)DatasetCompressionStats, METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

PyTypeObject DatasetType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
};

PyMethodDef ModuleMethods[] = {
    {"set_compression_enabled", (PyCFunction)SetCompressionEnabled, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef ModuleDef = {
    PyModuleDef_HEAD_INIT,
    "_mimicdb",
    nullptr,
    -1,
    ModuleMethods,
};

}  // namespace

PyMODINIT_FUNC PyInit__mimicdb() {
    DatasetType.tp_name = "mimicapi._mimicdb.Dataset";
    DatasetType.tp_basicsize = sizeof(DatasetObject);
    DatasetType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    DatasetType.tp_new = PyType_GenericNew;
    DatasetType.tp_init = reinterpret_cast<initproc>(DatasetInit);
    DatasetType.tp_dealloc = reinterpret_cast<destructor>(DatasetDealloc);
    DatasetType.tp_methods = DatasetMethods;
    if (PyType_Ready(&DatasetType) < 0) {
        return nullptr;
    }
    PyObject* module = PyModule_Create(&ModuleDef);
    if (!module) {
        return nullptr;
    }
    Py_INCREF(&DatasetType);
    if (PyModule_AddObject(module, "Dataset", reinterpret_cast<PyObject*>(&DatasetType)) < 0) {
        Py_DECREF(&DatasetType);
        Py_DECREF(module);
        return nullptr;
    }
    return module;
}
