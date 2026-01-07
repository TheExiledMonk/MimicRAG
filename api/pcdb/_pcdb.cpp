#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "pcdb/aggregate.h"
#include "pcdb/dataset.h"
#include "pcdb/field_vector.h"
#include "pcdb/mask.h"
#include "pcdb/predicate.h"
#include "pcdb/scan.h"
#include "pcdb/segment.h"
#include "pcdb/types.h"

namespace {

struct DatasetObject {
    PyObject_HEAD
    pcdb::Dataset* dataset = nullptr;
    std::vector<std::string>* field_names = nullptr;
    std::vector<pcdb::FieldType>* field_types = nullptr;
    std::unordered_map<std::string, size_t>* field_index = nullptr;
};

struct ParsedPredicate {
    size_t field_index = 0;
    pcdb::CompareOp op = pcdb::CompareOp::kEq;
    double value = 0.0;
};

pcdb::FieldType ParseFieldType(const std::string& type_name, bool* ok) {
    *ok = true;
    if (type_name == "int32") {
        return pcdb::FieldType::kInt32;
    }
    if (type_name == "int64") {
        return pcdb::FieldType::kInt64;
    }
    if (type_name == "float64") {
        return pcdb::FieldType::kFloat64;
    }
    if (type_name == "bool") {
        return pcdb::FieldType::kBool;
    }
    if (type_name == "dict_int32") {
        return pcdb::FieldType::kDictInt32;
    }
    if (type_name == "int16") {
        return pcdb::FieldType::kInt32;
    }
    *ok = false;
    return pcdb::FieldType::kInt32;
}

bool ParseCompareOp(const std::string& op, pcdb::CompareOp* out) {
    if (op == "eq") {
        *out = pcdb::CompareOp::kEq;
        return true;
    }
    if (op == "ne") {
        *out = pcdb::CompareOp::kNe;
        return true;
    }
    if (op == "lt") {
        *out = pcdb::CompareOp::kLt;
        return true;
    }
    if (op == "le") {
        *out = pcdb::CompareOp::kLe;
        return true;
    }
    if (op == "gt") {
        *out = pcdb::CompareOp::kGt;
        return true;
    }
    if (op == "ge") {
        *out = pcdb::CompareOp::kGe;
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
    self->dataset = new pcdb::Dataset(name);
    self->field_names = new std::vector<std::string>();
    self->field_types = new std::vector<pcdb::FieldType>();
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
        self->dataset->AddField(pcdb::FieldVector(field_name, type));
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
    std::vector<pcdb::FieldValue> values;
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
            values.push_back(pcdb::FieldValue::Null(type));
            continue;
        }
        if (type == pcdb::FieldType::kBool) {
            const int truth = PyObject_IsTrue(value);
            if (truth < 0) {
                return nullptr;
            }
            values.push_back(pcdb::FieldValue::Bool(truth != 0));
            continue;
        }
        if (type == pcdb::FieldType::kFloat64) {
            const double val = PyFloat_AsDouble(value);
            if (PyErr_Occurred()) {
                return nullptr;
            }
            values.push_back(pcdb::FieldValue::Float64(val));
            continue;
        }
        const long long val = PyLong_AsLongLong(value);
        if (PyErr_Occurred()) {
            return nullptr;
        }
        if (type == pcdb::FieldType::kInt32 || type == pcdb::FieldType::kDictInt32) {
            values.push_back(pcdb::FieldValue::Int32(static_cast<int32_t>(val)));
        } else {
            values.push_back(pcdb::FieldValue::Int64(static_cast<int64_t>(val)));
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
    std::vector<std::vector<uint8_t>> validity_buffers;
    i32_values.reserve(self->field_names->size());
    i64_values.reserve(self->field_names->size());
    f64_values.reserve(self->field_names->size());
    bool_values.reserve(self->field_names->size());
    validity_buffers.reserve(self->field_names->size());

    std::vector<pcdb::FieldBatch> batches;
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

        pcdb::FieldBatch batch;
        batch.type = (*self->field_types)[i];
        batch.count = count;

        std::vector<uint8_t> validity;
        validity.reserve(count);

        if (batch.type == pcdb::FieldType::kInt32 || batch.type == pcdb::FieldType::kDictInt32) {
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
        } else if (batch.type == pcdb::FieldType::kInt64) {
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
        } else if (batch.type == pcdb::FieldType::kFloat64) {
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
        } else if (batch.type == pcdb::FieldType::kBool) {
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
        pcdb::CompareOp op;
        if (!ParseCompareOp(op_name, &op)) {
            PyErr_SetString(PyExc_ValueError, "unsupported compare op");
            return nullptr;
        }
        const auto& field = self->dataset->Fields()[it->second];
        const auto type = field.Type();
        double value = 0.0;
        if (type == pcdb::FieldType::kBool) {
            const int truth = PyObject_IsTrue(value_obj);
            if (truth < 0) {
                return nullptr;
            }
            value = truth ? 1.0 : 0.0;
        } else if (type == pcdb::FieldType::kFloat64) {
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

    std::vector<const pcdb::Segment*> segments;
    segments.reserve(self->dataset->Segments().size() + 1);
    for (const auto& seg : self->dataset->Segments()) {
        segments.push_back(&seg);
    }
    pcdb::Segment active_segment(0, 0, {});
    if (self->dataset->ActiveRowCount() > 0) {
        active_segment = pcdb::Segment(self->dataset->SegmentCapacity(),
                                       self->dataset->ActiveRowCount(),
                                       self->dataset->ActiveFields());
        segments.push_back(&active_segment);
    }

    pcdb::Metrics metrics;
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

    pcdb::AggregateResult sum_acc;
    pcdb::AggregateResult minmax_acc;
    pcdb::AggregateResult mixed_acc;
    bool mixed_has_value = false;

    for (const auto* seg : segments) {
        bool matches = true;
        for (const auto& pred : predicates) {
            const auto& stats = seg->ColumnStats();
            if (pred.field_index >= stats.size() ||
                !pcdb::SegmentMatchesPredicate(stats[pred.field_index], pred.op, pred.value)) {
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
        pcdb::Mask mask(count);
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
                    case pcdb::FieldType::kInt32:
                        keep = pcdb::CompareInt64(field.DataInt32()[row],
                                                  static_cast<int64_t>(pred.value), pred.op);
                        break;
                    case pcdb::FieldType::kInt64:
                        keep = pcdb::CompareInt64(field.DataInt64()[row],
                                                  static_cast<int64_t>(pred.value), pred.op);
                        break;
                    case pcdb::FieldType::kFloat64:
                        keep = pcdb::CompareFloat64(field.DataFloat64()[row], pred.value, pred.op);
                        break;
                    case pcdb::FieldType::kBool:
                        keep = pcdb::CompareInt64(field.DataBool()[row] ? 1 : 0,
                                                  static_cast<int64_t>(pred.value), pred.op);
                        break;
                    case pcdb::FieldType::kDictInt32: {
                        const auto* dict = field.Dictionary();
                        const auto* ids = field.DataDictIds();
                        keep = pcdb::CompareInt64(dict->Value(ids[row]),
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
            pcdb::AggregateResult seg_result;
            pcdb::AggregateMixed(seg->Fields()[sum_index], &mask, &seg_result);
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
                pcdb::AggregateResult seg_sum;
                pcdb::AggregateSum(seg->Fields()[sum_index], &mask, &seg_sum);
                sum_acc.sum += seg_sum.sum;
                sum_acc.count += seg_sum.count;
            }
            if (has_min || has_max) {
                const size_t idx = has_min ? min_index : max_index;
                pcdb::AggregateResult seg_mm;
                pcdb::AggregateMinMax(seg->Fields()[idx], &mask, &seg_mm);
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

PyMethodDef DatasetMethods[] = {
    {"append", (PyCFunction)DatasetAppend, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"append_batch", (PyCFunction)DatasetAppendBatch, METH_VARARGS, nullptr},
    {"aggregate", (PyCFunction)DatasetAggregate, METH_VARARGS, nullptr},
    {"aggregate_debug", (PyCFunction)DatasetAggregateDebug, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

PyTypeObject DatasetType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
};

PyModuleDef ModuleDef = {
    PyModuleDef_HEAD_INIT,
    "_pcdb",
    nullptr,
    -1,
    nullptr,
};

}  // namespace

PyMODINIT_FUNC PyInit__pcdb() {
    DatasetType.tp_name = "pcdb._pcdb.Dataset";
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
