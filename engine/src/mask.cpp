#include "pcdb/mask.h"

namespace pcdb {

namespace {
constexpr size_t kWordBits = 64;
}

PackedMask::PackedMask(size_t bits) {
    Resize(bits);
}

void PackedMask::Resize(size_t bits) {
    bit_count_ = bits;
    const size_t word_count = (bits + kWordBits - 1) / kWordBits;
    words_.assign(word_count, 0);
}

size_t PackedMask::Size() const {
    return bit_count_;
}

bool PackedMask::Get(size_t index) const {
    if (index >= bit_count_) {
        return false;
    }
    const size_t word_index = index / kWordBits;
    const size_t bit_index = index % kWordBits;
    return (words_[word_index] >> bit_index) & 1ULL;
}

void PackedMask::Set(size_t index, bool value) {
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

const uint64_t* PackedMask::Words() const {
    return words_.data();
}

size_t PackedMask::WordCount() const {
    return words_.size();
}

Mask::Mask(size_t bits) {
    Resize(bits);
}

void Mask::Resize(size_t bits) {
    bit_count_ = bits;
    const size_t word_count = (bits + kWordBits - 1) / kWordBits;
    words_.assign(word_count, 0);
}

size_t Mask::Size() const {
    return bit_count_;
}

bool Mask::Get(size_t index) const {
    if (index >= bit_count_) {
        return false;
    }
    const size_t word_index = index / kWordBits;
    const size_t bit_index = index % kWordBits;
    return (words_[word_index] >> bit_index) & 1ULL;
}

void Mask::Set(size_t index, bool value) {
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

Mask Mask::And(const Mask& left, const Mask& right) {
    const size_t bits = left.bit_count_ < right.bit_count_ ? left.bit_count_ : right.bit_count_;
    Mask result(bits);
    const size_t word_count = result.words_.size();
    for (size_t i = 0; i < word_count; ++i) {
        const uint64_t left_word = i < left.words_.size() ? left.words_[i] : 0;
        const uint64_t right_word = i < right.words_.size() ? right.words_[i] : 0;
        result.words_[i] = left_word & right_word;
    }
    return result;
}

Mask Mask::Or(const Mask& left, const Mask& right) {
    const size_t bits = left.bit_count_ > right.bit_count_ ? left.bit_count_ : right.bit_count_;
    Mask result(bits);
    const size_t word_count = result.words_.size();
    for (size_t i = 0; i < word_count; ++i) {
        const uint64_t left_word = i < left.words_.size() ? left.words_[i] : 0;
        const uint64_t right_word = i < right.words_.size() ? right.words_[i] : 0;
        result.words_[i] = left_word | right_word;
    }
    return result;
}

Mask BuildMask(const Int64Predicate& predicate, size_t count) {
    Mask mask(count);
    if (!predicate.data || !predicate.compare) {
        return mask;
    }
    for (size_t i = 0; i < count; ++i) {
        mask.Set(i, predicate.compare(predicate.data[i], predicate.value));
    }
    return mask;
}

Mask BuildMaskBranchless(const Int64PredicateBranchless& predicate, size_t count) {
    Mask mask(count);
    if (!predicate.data) {
        return mask;
    }
    for (size_t i = 0; i < count; ++i) {
        const uint8_t keep = CompareInt64Branchless(predicate.data[i], predicate.value, predicate.op);
        mask.Set(i, keep != 0);
    }
    return mask;
}

PackedMask PackMask(const Mask& mask) {
    PackedMask packed(mask.Size());
    for (size_t i = 0; i < mask.Size(); ++i) {
        packed.Set(i, mask.Get(i));
    }
    return packed;
}

}  // namespace pcdb
