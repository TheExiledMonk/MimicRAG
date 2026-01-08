#ifndef MIMICDB_COMPRESSION_H
#define MIMICDB_COMPRESSION_H

#include <cstddef>
#include <cstdint>

#include <vector>

#include "mimicdb/dictionary.h"
#include "mimicdb/mask.h"
#include "mimicdb/predicate.h"
#include "mimicdb/types.h"

namespace mimicdb {

enum class ColumnCompressionKind : uint8_t {
    kNone,
    kBitPacked,
    kDictionary,
    kForDelta,
    kRle,
    kLz4,
};

struct CompressionPredicate {
    FieldType type = FieldType::kInt64;
    CompareOp op = CompareOp::kEq;
    bool is_null_check = false;
    bool is_not_null_check = false;
    bool null_is = true;
    int64_t i64 = 0;
    double f64 = 0.0;
    uint8_t bool_value = 0;
    const uint8_t* bytes = nullptr;
    uint32_t bytes_size = 0;
};

class FieldVector;
struct SegmentColumnStats;
struct CompressedColumnView;

using DecodeBatchFn = void (*)(const CompressedColumnView& column,
                               const Mask& mask,
                               void* out_buffer);
using ScanPredicateFn = void (*)(const CompressedColumnView& column,
                                 const CompressionPredicate& predicate,
                                 Mask* out_mask);
using MemoryBytesFn = size_t (*)(const CompressedColumnView& column);

struct CompressedColumnOps {
    DecodeBatchFn decode_batch = nullptr;
    ScanPredicateFn scan_predicate = nullptr;
    MemoryBytesFn memory_bytes = nullptr;
};

struct CompressedColumnView {
    ColumnCompressionKind kind = ColumnCompressionKind::kNone;
    FieldType type = FieldType::kInt64;
    const uint8_t* data = nullptr;
    size_t data_size = 0;
    const uint8_t* aux = nullptr;
    size_t aux_size = 0;
    size_t raw_data_size = 0;
    size_t raw_aux_size = 0;
    const DictionaryInt32* dict = nullptr;
    const uint64_t* validity_words = nullptr;
    size_t validity_word_count = 0;
    size_t validity_bit_count = 0;
    size_t row_count = 0;
    const void* codec_state = nullptr;
    const CompressedColumnOps* ops = nullptr;
};

struct CompressionConfig {
    bool enabled = true;
    bool enable_dictionary = true;
    bool enable_bitpack = true;
    bool enable_fordelta = true;
    bool enable_lz4 = true;
    size_t min_segment_rows = 1024;
    double min_ratio = 1.1;
};

inline void DecodeBatch(const CompressedColumnView& column,
                        const Mask& mask,
                        void* out_buffer) {
    if (column.ops && column.ops->decode_batch) {
        column.ops->decode_batch(column, mask, out_buffer);
    }
}

inline void ScanPredicate(const CompressedColumnView& column,
                          const CompressionPredicate& predicate,
                          Mask* out_mask) {
    if (column.ops && column.ops->scan_predicate && out_mask) {
        column.ops->scan_predicate(column, predicate, out_mask);
    }
}

inline size_t MemoryBytes(const CompressedColumnView& column) {
    if (column.ops && column.ops->memory_bytes) {
        return column.ops->memory_bytes(column);
    }
    return column.data_size + column.aux_size + column.validity_word_count * sizeof(uint64_t);
}

inline bool IsValid(const CompressedColumnView& column, size_t index) {
    if (!column.validity_words || column.validity_word_count == 0) {
        return true;
    }
    const size_t word = index / 64;
    const size_t bit = index % 64;
    if (word >= column.validity_word_count) {
        return true;
    }
    return (column.validity_words[word] >> bit) & 1ULL;
}

ColumnCompressionKind ChooseCompression(const SegmentColumnStats& stats, FieldType type);
CompressedColumnView MakeUncompressedView(const FieldVector& field);
const CompressedColumnOps* DefaultCompressionOps();
const CompressedColumnOps* Lz4CompressionOps();
bool EncodeLz4Literal(const uint8_t* src, size_t src_size, std::vector<uint8_t>* out);
bool DecodeLz4Literal(const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size);
CompressionConfig GetCompressionConfig();
void SetCompressionConfig(const CompressionConfig& config);

}  // namespace mimicdb

#endif  // MIMICDB_COMPRESSION_H
