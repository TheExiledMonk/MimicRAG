#ifndef PCDB_MASK_H
#define PCDB_MASK_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pcdb/predicate.h"

namespace pcdb {

class Mask {
public:
    Mask() = default;
    explicit Mask(size_t bits);

    void Resize(size_t bits);
    size_t Size() const;

    bool Get(size_t index) const;
    void Set(size_t index, bool value);

    static Mask And(const Mask& left, const Mask& right);
    static Mask Or(const Mask& left, const Mask& right);

private:
    std::vector<uint64_t> words_;
    size_t bit_count_ = 0;
};

class PackedMask {
public:
    PackedMask() = default;
    explicit PackedMask(size_t bits);

    void Resize(size_t bits);
    size_t Size() const;
    bool Get(size_t index) const;
    void Set(size_t index, bool value);
    const uint64_t* Words() const;
    size_t WordCount() const;

private:
    std::vector<uint64_t> words_;
    size_t bit_count_ = 0;
};

struct Int64Predicate {
    const int64_t* data = nullptr;
    int64_t value = 0;
    bool (*compare)(int64_t left, int64_t right) = nullptr;
};

struct Int64PredicateBranchless {
    const int64_t* data = nullptr;
    int64_t value = 0;
    CompareOp op = CompareOp::kEq;
};

Mask BuildMask(const Int64Predicate& predicate, size_t count);
Mask BuildMaskBranchless(const Int64PredicateBranchless& predicate, size_t count);
PackedMask PackMask(const Mask& mask);

}  // namespace pcdb

#endif  // PCDB_MASK_H
