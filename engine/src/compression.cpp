#include "mimicdb/compression.h"

#include <algorithm>
#include <cstring>

#include "mimicdb/config.h"
#include "mimicdb/field_vector.h"
#include "mimicdb/segment.h"

namespace mimicdb {

namespace {

constexpr size_t kLz4BlockSize = 64 * 1024;
constexpr uint8_t kBitpackHeaderSize = 16;
constexpr uint8_t kDictHeaderSize = 8;
CompressionConfig g_compression_config = []() {
    CompressionConfig cfg;
    cfg.min_segment_rows = Config::kDefaultSegmentRows;
    cfg.min_ratio = Config::kCompressionMinRatio;
    return cfg;
}();

struct DictHeader {
    uint32_t count = 0;
    uint32_t reserved = 0;
};

struct BitpackHeader {
    int64_t base = 0;
    uint8_t bits = 0;
    uint8_t reserved[7] = {};
};

struct ForDeltaHeader {
    int64_t base = 0;
};

bool ReadDictHeader(const CompressedColumnView& column, DictHeader* out,
                    const int64_t** values) {
    if (!out || !values || !column.aux || column.aux_size < kDictHeaderSize) {
        return false;
    }
    std::memcpy(out, column.aux, sizeof(DictHeader));
    const size_t expected =
        kDictHeaderSize + static_cast<size_t>(out->count) * sizeof(int64_t);
    if (column.aux_size < expected) {
        return false;
    }
    *values = reinterpret_cast<const int64_t*>(column.aux + kDictHeaderSize);
    return true;
}

bool ReadBitpackHeader(const CompressedColumnView& column, BitpackHeader* out) {
    if (!out || !column.aux || column.aux_size < kBitpackHeaderSize) {
        return false;
    }
    std::memcpy(out, column.aux, sizeof(BitpackHeader));
    return out->bits > 0 && out->bits <= 32;
}

bool ReadForDeltaHeader(const CompressedColumnView& column, ForDeltaHeader* out) {
    if (!out || !column.aux || column.aux_size < sizeof(ForDeltaHeader)) {
        return false;
    }
    std::memcpy(out, column.aux, sizeof(ForDeltaHeader));
    return true;
}

uint64_t ReadBits(const uint8_t* data, size_t bit_offset, uint8_t bits) {
    if (!data || bits == 0) {
        return 0;
    }
    uint64_t value = 0;
    for (uint8_t i = 0; i < bits; ++i) {
        const size_t bit = bit_offset + i;
        const size_t byte = bit / 8;
        const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8));
        if (data[byte] & mask) {
            value |= (1ULL << i);
        }
    }
    return value;
}

bool ReadBitpackValueFast(const uint8_t* data, size_t index, const BitpackHeader& header,
                          uint64_t* out) {
    if (!data || !out) {
        return false;
    }
    switch (header.bits) {
        case 4: {
            const uint8_t byte = data[index >> 1];
            const uint8_t nibble = (index & 1u) ? (byte >> 4) : (byte & 0x0F);
            *out = nibble;
            return true;
        }
        case 8:
            *out = data[index];
            return true;
        case 16: {
            uint16_t value = 0;
            std::memcpy(&value, data + index * sizeof(uint16_t), sizeof(uint16_t));
            *out = value;
            return true;
        }
        case 32: {
            uint32_t value = 0;
            std::memcpy(&value, data + index * sizeof(uint32_t), sizeof(uint32_t));
            *out = value;
            return true;
        }
        default:
            break;
    }
    return false;
}

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

bool ReadInt64ValueInternal(const CompressedColumnView& column, size_t index, int64_t* out) {
    if (!out || !IsValid(column, index)) {
        return false;
    }
    switch (column.kind) {
        case ColumnCompressionKind::kNone:
        case ColumnCompressionKind::kLz4:
            break;
        case ColumnCompressionKind::kDictionary: {
            DictHeader header;
            const int64_t* values = nullptr;
            if (!ReadDictHeader(column, &header, &values)) {
                return false;
            }
            const auto* ids = reinterpret_cast<const uint32_t*>(column.data);
            if (!ids || ids[index] >= header.count) {
                return false;
            }
            *out = values[ids[index]];
            return true;
        }
        case ColumnCompressionKind::kBitPacked: {
            BitpackHeader header;
            if (!ReadBitpackHeader(column, &header)) {
                return false;
            }
            const uint64_t delta = ReadBits(column.data, index * header.bits, header.bits);
            *out = header.base + static_cast<int64_t>(delta);
            return true;
        }
        case ColumnCompressionKind::kForDelta: {
            ForDeltaHeader header;
            if (!ReadForDeltaHeader(column, &header)) {
                return false;
            }
            const auto* deltas = reinterpret_cast<const uint32_t*>(column.data);
            if (!deltas) {
                return false;
            }
            *out = header.base + static_cast<int64_t>(deltas[index]);
            return true;
        }
        case ColumnCompressionKind::kRle:
            return false;
    }
    switch (column.type) {
        case FieldType::kInt32:
            *out = static_cast<int64_t>(
                reinterpret_cast<const int32_t*>(column.data)[index]);
            return true;
        case FieldType::kInt64:
            *out = reinterpret_cast<const int64_t*>(column.data)[index];
            return true;
        case FieldType::kBool:
            *out = reinterpret_cast<const uint8_t*>(column.data)[index] ? 1 : 0;
            return true;
        case FieldType::kDictInt32: {
            const auto* ids = reinterpret_cast<const uint32_t*>(column.data);
            if (column.dict) {
                *out = static_cast<int64_t>(column.dict->Value(ids[index]));
            } else {
                *out = static_cast<int64_t>(ids[index]);
            }
            return true;
        }
        case FieldType::kFloat64:
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
        case FieldType::kObject:
            return false;
    }
    return false;
}

void DecodeDictionary(const CompressedColumnView& column, const Mask& mask, void* out_buffer) {
    if (!out_buffer || !column.data) {
        return;
    }
    auto* out = reinterpret_cast<int64_t*>(out_buffer);
    DictHeader header;
    const int64_t* values = nullptr;
    if (!ReadDictHeader(column, &header, &values)) {
        return;
    }
    const auto* ids = reinterpret_cast<const uint32_t*>(column.data);
    const bool dense_ids = header.count <= 256;
    for (size_t i = 0; i < column.row_count; ++i) {
        if (mask.Size() > 0 && !mask.Get(i)) {
            continue;
        }
        if (!IsValid(column, i) || !ids || (!dense_ids && ids[i] >= header.count)) {
            out[i] = 0;
            continue;
        }
        out[i] = values[ids[i]];
    }
}

void DecodeBitpack(const CompressedColumnView& column, const Mask& mask, void* out_buffer) {
    if (!out_buffer || !column.data) {
        return;
    }
    BitpackHeader header;
    if (!ReadBitpackHeader(column, &header)) {
        return;
    }
    auto* out = reinterpret_cast<int64_t*>(out_buffer);
    const bool has_mask = mask.Size() > 0;
    if (!has_mask && (header.bits == 4 || header.bits == 8 || header.bits == 16 ||
                      header.bits == 32)) {
        for (size_t i = 0; i < column.row_count; ++i) {
            uint64_t delta = 0;
            if (!ReadBitpackValueFast(column.data, i, header, &delta)) {
                delta = ReadBits(column.data, i * header.bits, header.bits);
            }
            out[i] = header.base + static_cast<int64_t>(delta);
        }
        return;
    }
    for (size_t i = 0; i < column.row_count; ++i) {
        if (has_mask && !mask.Get(i)) {
            continue;
        }
        uint64_t delta = 0;
        if (!ReadBitpackValueFast(column.data, i, header, &delta)) {
            delta = ReadBits(column.data, i * header.bits, header.bits);
        }
        out[i] = header.base + static_cast<int64_t>(delta);
    }
}

void DecodeForDelta(const CompressedColumnView& column, const Mask& mask, void* out_buffer) {
    if (!out_buffer || !column.data) {
        return;
    }
    ForDeltaHeader header;
    if (!ReadForDeltaHeader(column, &header)) {
        return;
    }
    const auto* deltas = reinterpret_cast<const uint32_t*>(column.data);
    auto* out = reinterpret_cast<int64_t*>(out_buffer);
    const bool has_mask = mask.Size() > 0;
    if (!has_mask) {
        size_t i = 0;
        for (; i + 3 < column.row_count; i += 4) {
            out[i] = header.base + static_cast<int64_t>(deltas[i]);
            out[i + 1] = header.base + static_cast<int64_t>(deltas[i + 1]);
            out[i + 2] = header.base + static_cast<int64_t>(deltas[i + 2]);
            out[i + 3] = header.base + static_cast<int64_t>(deltas[i + 3]);
        }
        for (; i < column.row_count; ++i) {
            out[i] = header.base + static_cast<int64_t>(deltas[i]);
        }
        return;
    }
    for (size_t i = 0; i < column.row_count; ++i) {
        if (!mask.Get(i)) {
            continue;
        }
        out[i] = header.base + static_cast<int64_t>(deltas[i]);
    }
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

void ScanPredicateDictionary(const CompressedColumnView& column,
                             const CompressionPredicate& predicate,
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
    DictHeader header;
    const int64_t* values = nullptr;
    if (!ReadDictHeader(column, &header, &values)) {
        return;
    }
    const auto* ids = reinterpret_cast<const uint32_t*>(column.data);
    const bool dense_ids = header.count <= 256;
    uint32_t match_id = 0;
    bool has_id = false;
    if (predicate.op == CompareOp::kEq || predicate.op == CompareOp::kNe) {
        for (uint32_t i = 0; i < header.count; ++i) {
            if (values[i] == predicate.i64) {
                match_id = i;
                has_id = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < column.row_count; ++i) {
        if (!IsValid(column, i)) {
            out_mask->Set(i, false);
            continue;
        }
        if ((predicate.op == CompareOp::kEq || predicate.op == CompareOp::kNe) && !has_id) {
            out_mask->Set(i, predicate.op == CompareOp::kNe);
            continue;
        }
        int64_t value = 0;
        if (predicate.op == CompareOp::kEq || predicate.op == CompareOp::kNe) {
            const bool keep = predicate.op == CompareOp::kEq
                ? ids[i] == match_id
                : ids[i] != match_id;
            out_mask->Set(i, keep);
            continue;
        }
        if (!ids || (!dense_ids && ids[i] >= header.count)) {
            out_mask->Set(i, false);
            continue;
        }
        value = values[ids[i]];
        const uint8_t keep =
            CompareInt64Branchless(value, predicate.i64, predicate.op);
        out_mask->Set(i, keep != 0);
    }
}

void ScanPredicateBitpack(const CompressedColumnView& column,
                          const CompressionPredicate& predicate,
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
    BitpackHeader header;
    if (!ReadBitpackHeader(column, &header)) {
        return;
    }
    const bool fast_bits = header.bits == 4 || header.bits == 8 ||
                           header.bits == 16 || header.bits == 32;
    for (size_t i = 0; i < column.row_count; ++i) {
        if (!IsValid(column, i)) {
            out_mask->Set(i, false);
            continue;
        }
        uint64_t delta = 0;
        if (fast_bits) {
            if (!ReadBitpackValueFast(column.data, i, header, &delta)) {
                delta = ReadBits(column.data, i * header.bits, header.bits);
            }
        } else {
            delta = ReadBits(column.data, i * header.bits, header.bits);
        }
        const int64_t value = header.base + static_cast<int64_t>(delta);
        const uint8_t keep =
            CompareInt64Branchless(value, predicate.i64, predicate.op);
        out_mask->Set(i, keep != 0);
    }
}

void ScanPredicateForDelta(const CompressedColumnView& column,
                           const CompressionPredicate& predicate,
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
    ForDeltaHeader header;
    if (!ReadForDeltaHeader(column, &header)) {
        return;
    }
    const auto* deltas = reinterpret_cast<const uint32_t*>(column.data);
    size_t i = 0;
    for (; i + 3 < column.row_count; i += 4) {
        for (size_t j = 0; j < 4; ++j) {
            const size_t idx = i + j;
            if (!IsValid(column, idx)) {
                out_mask->Set(idx, false);
                continue;
            }
            const int64_t value = header.base + static_cast<int64_t>(deltas[idx]);
            const uint8_t keep =
                CompareInt64Branchless(value, predicate.i64, predicate.op);
            out_mask->Set(idx, keep != 0);
        }
    }
    for (; i < column.row_count; ++i) {
        if (!IsValid(column, i)) {
            out_mask->Set(i, false);
            continue;
        }
        const int64_t value = header.base + static_cast<int64_t>(deltas[i]);
        const uint8_t keep =
            CompareInt64Branchless(value, predicate.i64, predicate.op);
        out_mask->Set(i, keep != 0);
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

const CompressedColumnOps kDictionaryOps = {
    &DecodeDictionary,
    &ScanPredicateDictionary,
    &MemoryBytesNone,
};

const CompressedColumnOps kBitpackOps = {
    &DecodeBitpack,
    &ScanPredicateBitpack,
    &MemoryBytesNone,
};

const CompressedColumnOps kForDeltaOps = {
    &DecodeForDelta,
    &ScanPredicateForDelta,
    &MemoryBytesNone,
};

const CompressedColumnOps kLz4Ops = {
    &DecodeLz4,
    nullptr,
    &MemoryBytesLz4,
};

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

const CompressedColumnOps* CompressionOpsFor(ColumnCompressionKind kind) {
    switch (kind) {
        case ColumnCompressionKind::kNone:
            return &kNoneOps;
        case ColumnCompressionKind::kDictionary:
            return &kDictionaryOps;
        case ColumnCompressionKind::kBitPacked:
            return &kBitpackOps;
        case ColumnCompressionKind::kForDelta:
            return &kForDeltaOps;
        case ColumnCompressionKind::kLz4:
            return &kLz4Ops;
        case ColumnCompressionKind::kRle:
            return &kNoneOps;
    }
    return &kNoneOps;
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

bool ReadInt64Value(const CompressedColumnView& column, size_t index, int64_t* out) {
    return ReadInt64ValueInternal(column, index, out);
}

bool ReadNumericValue(const CompressedColumnView& column, size_t index, double* out) {
    if (!out || !IsValid(column, index)) {
        return false;
    }
    if (column.kind == ColumnCompressionKind::kDictionary ||
        column.kind == ColumnCompressionKind::kBitPacked ||
        column.kind == ColumnCompressionKind::kForDelta) {
        int64_t value = 0;
        if (!ReadInt64ValueInternal(column, index, &value)) {
            return false;
        }
        *out = static_cast<double>(value);
        return true;
    }
    switch (column.type) {
        case FieldType::kInt32:
            *out = static_cast<double>(
                reinterpret_cast<const int32_t*>(column.data)[index]);
            return true;
        case FieldType::kInt64:
            *out = static_cast<double>(
                reinterpret_cast<const int64_t*>(column.data)[index]);
            return true;
        case FieldType::kFloat64:
            *out = reinterpret_cast<const double*>(column.data)[index];
            return true;
        case FieldType::kBool:
            *out = reinterpret_cast<const uint8_t*>(column.data)[index] != 0;
            return true;
        case FieldType::kDictInt32:
            if (column.dict) {
                *out = static_cast<double>(
                    column.dict->Value(reinterpret_cast<const uint32_t*>(column.data)[index]));
            } else {
                *out = static_cast<double>(
                    reinterpret_cast<const uint32_t*>(column.data)[index]);
            }
            return true;
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
        case FieldType::kObject:
            return false;
    }
    return false;
}

CompressionConfig GetCompressionConfig() {
    return g_compression_config;
}

void SetCompressionConfig(const CompressionConfig& config) {
    g_compression_config = config;
}

}  // namespace mimicdb
