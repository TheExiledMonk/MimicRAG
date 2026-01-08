#include "mimicdb/bitmap.h"

namespace mimicdb {

namespace {
constexpr size_t kWordBits = 64;
}

Bitmap::Bitmap(size_t bits) {
    Resize(bits);
}

void Bitmap::Resize(size_t bits) {
    const size_t word_count = (bits + kWordBits - 1) / kWordBits;
    words_.resize(word_count, 0);
    bit_count_ = bits;
}

bool Bitmap::Get(size_t index) const {
    if (index >= bit_count_) {
        return false;
    }
    const size_t word_index = index / kWordBits;
    const size_t bit_index = index % kWordBits;
    return (words_[word_index] >> bit_index) & 1ULL;
}

void Bitmap::Set(size_t index, bool value) {
    if (index >= bit_count_) {
        return;
    }
    const size_t word_index = index / kWordBits;
    const size_t bit_index = index % kWordBits;
    const uint64_t mask = 1ULL << bit_index;
    if (value) {
        words_[word_index] |= mask;
    } else {
        words_[word_index] &= ~mask;
    }
}

size_t Bitmap::Size() const {
    return bit_count_;
}

const uint64_t* Bitmap::Words() const {
    return words_.data();
}

size_t Bitmap::WordCount() const {
    return words_.size();
}

void Bitmap::LoadWords(const uint64_t* words, size_t word_count, size_t bit_count) {
    words_.assign(words, words + word_count);
    bit_count_ = bit_count;
}

void Bitmap::AppendAllTrue(size_t count) {
    const size_t start = bit_count_;
    Resize(bit_count_ + count);
    for (size_t i = 0; i < count; ++i) {
        Set(start + i, true);
    }
}

void Bitmap::AppendBits(const uint8_t* validity, size_t count) {
    const size_t start = bit_count_;
    Resize(bit_count_ + count);
    for (size_t i = 0; i < count; ++i) {
        Set(start + i, validity[i] != 0);
    }
}

}  // namespace mimicdb
