#include "mimicdb/compression.h"

#include <cstring>

#include "mimicdb/config.h"
#include "mimicdb/field_vector.h"
#include "mimicdb/segment.h"

namespace mimicdb {

namespace {

constexpr size_t kLz4BlockSize = 64 * 1024;
CompressionConfig g_compression_config = []() {
    CompressionConfig cfg;
    cfg.min_segment_rows = Config::kDefaultSegmentRows;
    cfg.min_ratio = Config::kCompressionMinRatio;
    return cfg;
}();

bool EncodeLz4LiteralBlock(const uint8_t* src, size_t src_size,
                           std::vector<uint8_t>* out) {
    if (!out) {
        return false;
    }
    size_t offset = 0;
    while (offset < src_size) {
        const size_t block = (src_size - offset) < kLz4BlockSize
            ? (src_size - offset)
            : kLz4BlockSize;
        uint8_t token = 0;
        size_t literal_len = block;
        if (literal_len < 15) {
            token = static_cast<uint8_t>(literal_len << 4);
        } else {
            token = 15 << 4;
        }
        out->push_back(token);
        if (literal_len >= 15) {
            size_t remaining = literal_len - 15;
            while (remaining >= 255) {
                out->push_back(255);
                remaining -= 255;
            }
            out->push_back(static_cast<uint8_t>(remaining));
        }
        out->insert(out->end(), src + offset, src + offset + block);
        offset += block;
    }
    return true;
}

bool DecodeLz4LiteralBlock(const uint8_t* src, size_t src_size,
                           uint8_t* dst, size_t dst_size) {
    if (!dst && dst_size > 0) {
        return false;
    }
    size_t src_offset = 0;
    size_t dst_offset = 0;
    while (src_offset < src_size && dst_offset < dst_size) {
        const uint8_t token = src[src_offset++];
        size_t literal_len = token >> 4;
        if (literal_len == 15) {
            while (src_offset < src_size) {
                const uint8_t byte = src[src_offset++];
                literal_len += byte;
                if (byte != 255) {
                    break;
                }
            }
        }
        if (src_offset + literal_len > src_size || dst_offset + literal_len > dst_size) {
            return false;
        }
        std::memcpy(dst + dst_offset, src + src_offset, literal_len);
        src_offset += literal_len;
        dst_offset += literal_len;
    }
    return dst_offset == dst_size;
}

void DecodeNone(const CompressedColumnView& column, const Mask& mask, void* out_buffer) {
    (void)mask;
    if (!out_buffer || !column.data || column.data_size == 0) {
        return;
    }
    std::memcpy(out_buffer, column.data, column.data_size);
}

void ScanPredicateNone(const CompressedColumnView& column, const CompressionPredicate& predicate,
                       Mask* out_mask) {
    if (!out_mask) {
        return;
    }
    out_mask->Resize(column.row_count);
    if (!column.data || column.row_count == 0) {
        return;
    }
    if (predicate.is_null_check || predicate.is_not_null_check) {
        for (size_t i = 0; i < column.row_count; ++i) {
            const bool is_null = !IsValid(column, i);
            const bool keep = predicate.is_not_null_check
                                  ? !is_null
                                  : (predicate.null_is ? is_null : !is_null);
            out_mask->Set(i, keep);
        }
        return;
    }
    switch (column.type) {
        case FieldType::kInt32: {
            const auto* values = reinterpret_cast<const int32_t*>(column.data);
            for (size_t i = 0; i < column.row_count; ++i) {
                if (!IsValid(column, i)) {
                    out_mask->Set(i, false);
                    continue;
                }
                const uint8_t keep = CompareInt64Branchless(
                    static_cast<int64_t>(values[i]), predicate.i64, predicate.op);
                out_mask->Set(i, keep != 0);
            }
            break;
        }
        case FieldType::kInt64: {
            const auto* values = reinterpret_cast<const int64_t*>(column.data);
            for (size_t i = 0; i < column.row_count; ++i) {
                if (!IsValid(column, i)) {
                    out_mask->Set(i, false);
                    continue;
                }
                const uint8_t keep =
                    CompareInt64Branchless(values[i], predicate.i64, predicate.op);
                out_mask->Set(i, keep != 0);
            }
            break;
        }
        case FieldType::kFloat64: {
            const auto* values = reinterpret_cast<const double*>(column.data);
            for (size_t i = 0; i < column.row_count; ++i) {
                if (!IsValid(column, i)) {
                    out_mask->Set(i, false);
                    continue;
                }
                const uint8_t keep =
                    CompareFloat64Branchless(values[i], predicate.f64, predicate.op);
                out_mask->Set(i, keep != 0);
            }
            break;
        }
        case FieldType::kBool: {
            const auto* values = reinterpret_cast<const uint8_t*>(column.data);
            for (size_t i = 0; i < column.row_count; ++i) {
                if (!IsValid(column, i)) {
                    out_mask->Set(i, false);
                    continue;
                }
                const uint8_t keep = CompareInt64Branchless(
                    static_cast<int64_t>(values[i]), predicate.i64, predicate.op);
                out_mask->Set(i, keep != 0);
            }
            break;
        }
        case FieldType::kDictInt32: {
            const auto* values = reinterpret_cast<const uint32_t*>(column.data);
            uint32_t dict_id = 0;
            const bool has_dict_id = column.dict &&
                column.dict->FindId(static_cast<int32_t>(predicate.i64), &dict_id);
            for (size_t i = 0; i < column.row_count; ++i) {
                if (!IsValid(column, i)) {
                    out_mask->Set(i, false);
                    continue;
                }
                if ((predicate.op == CompareOp::kEq || predicate.op == CompareOp::kNe) &&
                    column.dict) {
                    if (!has_dict_id) {
                        out_mask->Set(i, predicate.op == CompareOp::kNe);
                        continue;
                    }
                    const bool keep = predicate.op == CompareOp::kEq
                        ? values[i] == dict_id
                        : values[i] != dict_id;
                    out_mask->Set(i, keep);
                    continue;
                }
                int64_t value = static_cast<int64_t>(values[i]);
                if (column.dict) {
                    value = static_cast<int64_t>(column.dict->Value(values[i]));
                }
                const uint8_t keep =
                    CompareInt64Branchless(value, predicate.i64, predicate.op);
                out_mask->Set(i, keep != 0);
            }
            break;
        }
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
        case FieldType::kObject:
            for (size_t i = 0; i < column.row_count; ++i) {
                out_mask->Set(i, false);
            }
            break;
    }
}

size_t MemoryBytesNone(const CompressedColumnView& column) {
    return column.data_size + column.aux_size;
}

void DecodeLz4(const CompressedColumnView& column, const Mask&, void* out_buffer) {
    if (!out_buffer || !column.data) {
        return;
    }
    DecodeLz4LiteralBlock(column.data, column.data_size,
                          reinterpret_cast<uint8_t*>(out_buffer),
                          column.raw_data_size);
}

size_t MemoryBytesLz4(const CompressedColumnView& column) {
    return column.data_size + column.aux_size;
}

const CompressedColumnOps kNoneOps = {
    &DecodeNone,
    &ScanPredicateNone,
    &MemoryBytesNone,
};

const CompressedColumnOps kLz4Ops = {
    &DecodeLz4,
    nullptr,
    &MemoryBytesLz4,
};

bool IsCodecEnabled(ColumnCompressionKind kind) {
    const auto cfg = g_compression_config;
    switch (kind) {
        case ColumnCompressionKind::kNone:
            return true;
        case ColumnCompressionKind::kDictionary:
            return cfg.enable_dictionary;
        case ColumnCompressionKind::kBitPacked:
            return cfg.enable_bitpack;
        case ColumnCompressionKind::kForDelta:
            return cfg.enable_fordelta;
        case ColumnCompressionKind::kLz4:
            return cfg.enable_lz4;
        case ColumnCompressionKind::kRle:
            return false;
    }
    return false;
}

}  // namespace

ColumnCompressionKind ChooseCompression(const SegmentColumnStats& stats, FieldType type) {
    const auto cfg = g_compression_config;
    if (!cfg.enabled) {
        return ColumnCompressionKind::kNone;
    }
    if (stats.value_count < cfg.min_segment_rows) {
        return ColumnCompressionKind::kNone;
    }
    if (!stats.has_value || stats.value_count == 0) {
        return ColumnCompressionKind::kNone;
    }
    switch (type) {
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
        case FieldType::kObject:
            return ColumnCompressionKind::kLz4;
        case FieldType::kInt32:
        case FieldType::kInt64:
        case FieldType::kBool:
        case FieldType::kDictInt32:
            if (cfg.enable_dictionary && stats.estimated_cardinality > 0 &&
                stats.estimated_cardinality <= Config::kCompressionDictCardinalityThreshold) {
                return ColumnCompressionKind::kDictionary;
            }
            break;
        case FieldType::kFloat64:
            break;
    }

    const double range = stats.max - stats.min;
    const double safe_range = range < 0.0 ? 0.0 : range;
    if (type == FieldType::kInt32 || type == FieldType::kInt64 || type == FieldType::kBool ||
        type == FieldType::kDictInt32) {
        const uint64_t max_range =
            safe_range > static_cast<double>(UINT64_MAX) ? UINT64_MAX
                                                        : static_cast<uint64_t>(safe_range);
        uint64_t bit_limit = 1;
        const uint8_t max_bits = Config::kCompressionBitpackMaxBits;
        for (uint8_t bits = 1; bits <= max_bits; bits *= 2) {
            if (bits == 1) {
                bit_limit = 1;
            } else {
                bit_limit = (1ULL << bits) - 1ULL;
            }
            if (max_range <= bit_limit) {
                if (cfg.enable_bitpack) {
                    return ColumnCompressionKind::kBitPacked;
                }
                break;
            }
        }
    }

    if (cfg.enable_fordelta &&
        safe_range <= static_cast<double>(Config::kCompressionForDeltaMaxRange)) {
        return ColumnCompressionKind::kForDelta;
    }

    return cfg.enable_lz4 ? ColumnCompressionKind::kLz4 : ColumnCompressionKind::kNone;
}

CompressedColumnView MakeUncompressedView(const FieldVector& field) {
    CompressedColumnView view;
    view.kind = ColumnCompressionKind::kNone;
    view.type = field.Type();
    view.row_count = field.Size();
    view.ops = &kNoneOps;
    if (field.HasNulls()) {
        view.validity_words = field.Validity().Words();
        view.validity_word_count = field.Validity().WordCount();
        view.validity_bit_count = field.Validity().Size();
    }
    switch (field.Type()) {
        case FieldType::kInt32:
            view.data = reinterpret_cast<const uint8_t*>(field.DataInt32());
            view.data_size = field.Size() * sizeof(int32_t);
            view.raw_data_size = view.data_size;
            break;
        case FieldType::kInt64:
            view.data = reinterpret_cast<const uint8_t*>(field.DataInt64());
            view.data_size = field.Size() * sizeof(int64_t);
            view.raw_data_size = view.data_size;
            break;
        case FieldType::kFloat64:
            view.data = reinterpret_cast<const uint8_t*>(field.DataFloat64());
            view.data_size = field.Size() * sizeof(double);
            view.raw_data_size = view.data_size;
            break;
        case FieldType::kBool:
            view.data = reinterpret_cast<const uint8_t*>(field.DataBool());
            view.data_size = field.Size() * sizeof(uint8_t);
            view.raw_data_size = view.data_size;
            break;
        case FieldType::kDictInt32:
            view.data = reinterpret_cast<const uint8_t*>(field.DataDictIds());
            view.data_size = field.Size() * sizeof(uint32_t);
            view.raw_data_size = view.data_size;
            view.dict = field.Dictionary();
            break;
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
            view.aux = reinterpret_cast<const uint8_t*>(field.DataLengths());
            view.aux_size = field.Size() * sizeof(uint32_t);
            view.data = reinterpret_cast<const uint8_t*>(field.DataBytes());
            view.data_size = field.BytesSize();
            view.raw_data_size = view.data_size;
            view.raw_aux_size = view.aux_size;
            break;
        case FieldType::kObject:
            view.data = nullptr;
            view.data_size = 0;
            break;
    }
    return view;
}

const CompressedColumnOps* DefaultCompressionOps() {
    return &kNoneOps;
}

const CompressedColumnOps* Lz4CompressionOps() {
    return &kLz4Ops;
}

bool EncodeLz4Literal(const uint8_t* src, size_t src_size, std::vector<uint8_t>* out) {
    if (!out) {
        return false;
    }
    out->clear();
    if (src_size == 0) {
        return true;
    }
    return EncodeLz4LiteralBlock(src, src_size, out);
}

bool DecodeLz4Literal(const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size) {
    if (dst_size == 0) {
        return true;
    }
    return DecodeLz4LiteralBlock(src, src_size, dst, dst_size);
}

CompressionConfig GetCompressionConfig() {
    return g_compression_config;
}

void SetCompressionConfig(const CompressionConfig& config) {
    g_compression_config = config;
}

}  // namespace mimicdb
