#define PY_SSIZE_T_CLEAN
#include <Python.h>

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
        predicates.push_back({it->second, op, value});
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

    for (const auto* seg : segments) {
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
            continue;
        }
        if (debug) {
            metrics.AddSegmentsScanned(1);
        }
        const size_t count = seg->RowCount();
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
                        keep = mimicdb::CompareFloat64(field.DataFloat64()[row], pred.value, pred.op);
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
                }
                if (!keep) {
                    mask.Set(row, false);
                }
            }
        }

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
        if (need_mixed) {
            PyDict_SetItemString(result, "count",
                                 PyLong_FromUnsignedLongLong(mixed_acc.count));
        } else if (has_sum) {
            PyDict_SetItemString(result, "count",
                                 PyLong_FromUnsignedLongLong(sum_acc.count));
        } else if (has_min || has_max) {
            PyDict_SetItemString(result, "count",
                                 PyLong_FromUnsignedLongLong(minmax_acc.count));
        } else {
            PyDict_SetItemString(result, "count",
                                 PyLong_FromUnsignedLongLong(0));
        }
    }

    if (debug) {
        PyObject* stats = PyDict_New();
        PyDict_SetItemString(stats, "segments_total",
                             PyLong_FromUnsignedLongLong(metrics.segments_total));
        PyDict_SetItemString(stats, "segments_scanned",
                             PyLong_FromUnsignedLongLong(metrics.segments_scanned));
        PyDict_SetItemString(stats, "segments_pruned",
                             PyLong_FromUnsignedLongLong(metrics.segments_pruned));
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
        predicates.push_back({it->second, op, value});
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

    PyObject* rows = PyList_New(0);
    if (!rows) {
        return nullptr;
    }
    uint64_t skipped = 0;
    uint64_t added = 0;
    bool stop = false;

    for (const auto* seg : segments) {
        const size_t count = seg->RowCount();
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
            PyObject* row_obj = PyDict_New();
            if (!row_obj) {
                Py_DECREF(rows);
                return nullptr;
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
    }

    return rows;
}

PyMethodDef DatasetMethods[] = {
    {"append", (PyCFunction)DatasetAppend, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"append_batch", (PyCFunction)DatasetAppendBatch, METH_VARARGS, nullptr},
    {"aggregate", (PyCFunction)DatasetAggregate, METH_VARARGS, nullptr},
    {"aggregate_debug", (PyCFunction)DatasetAggregateDebug, METH_VARARGS, nullptr},
    {"scan", (PyCFunction)DatasetScan, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

PyTypeObject DatasetType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
};

PyModuleDef ModuleDef = {
    PyModuleDef_HEAD_INIT,
    "_mimicdb",
    nullptr,
    -1,
    nullptr,
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
