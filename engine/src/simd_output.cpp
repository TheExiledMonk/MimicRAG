#include "mimicdb/simd_output.h"

#include <cstdint>

#include "mimicdb/simd.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace mimicdb {

namespace {
void CompressStorePackedScalar(const PackedMask& mask, size_t count, std::vector<size_t>* out) {
    if (!out) {
        return;
    }
    out->clear();
    const uint64_t* words = mask.Words();
    const size_t word_count = mask.WordCount();
    for (size_t word_index = 0; word_index < word_count; ++word_index) {
        uint64_t word = words[word_index];
        while (word) {
            const unsigned long bit = static_cast<unsigned long>(__builtin_ctzll(word));
            const size_t index = word_index * 64 + static_cast<size_t>(bit);
            if (index >= count) {
                break;
            }
            out->push_back(index);
            word &= word - 1;
        }
    }
}
#if defined(__AVX2__)
void CompressStorePackedAvx2(const PackedMask& mask, size_t count, std::vector<size_t>* out) {
    if (!out) {
        return;
    }
    out->clear();
    const uint64_t* words = mask.Words();
    const size_t word_count = mask.WordCount();
    size_t word_index = 0;
    for (; word_index + 4 <= word_count; word_index += 4) {
        const __m256i chunk = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(words + word_index));
        if (_mm256_testz_si256(chunk, chunk)) {
            continue;
        }
        for (size_t lane = 0; lane < 4; ++lane) {
            uint64_t word = words[word_index + lane];
            while (word) {
                const unsigned long bit = static_cast<unsigned long>(__builtin_ctzll(word));
                const size_t index = (word_index + lane) * 64 + static_cast<size_t>(bit);
                if (index >= count) {
                    break;
                }
                out->push_back(index);
                word &= word - 1;
            }
        }
    }
    for (; word_index < word_count; ++word_index) {
        uint64_t word = words[word_index];
        while (word) {
            const unsigned long bit = static_cast<unsigned long>(__builtin_ctzll(word));
            const size_t index = word_index * 64 + static_cast<size_t>(bit);
            if (index >= count) {
                break;
            }
            out->push_back(index);
            word &= word - 1;
        }
    }
}
#endif
}  // namespace

void CompressStoreScalar(const Mask& mask, size_t count, std::vector<size_t>* out) {
    if (!out) {
        return;
    }
    out->clear();
    for (size_t i = 0; i < count; ++i) {
        if (mask.Get(i)) {
            out->push_back(i);
        }
    }
}

void CompressStorePacked(const PackedMask& mask, size_t count, std::vector<size_t>* out) {
#if defined(__AVX2__)
    if (CpuHasAvx2()) {
        CompressStorePackedAvx2(mask, count, out);
        return;
    }
#endif
    CompressStorePackedScalar(mask, count, out);
}

}  // namespace mimicdb
