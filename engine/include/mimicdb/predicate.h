#ifndef MIMICDB_PREDICATE_H
#define MIMICDB_PREDICATE_H

#include <cstddef>
#include <cstdint>

namespace mimicdb {

enum class CompareOp : uint8_t {
    kEq,
    kNe,
    kLt,
    kLe,
    kGt,
    kGe,
};

bool CompareInt64(int64_t left, int64_t right, CompareOp op);
bool CompareFloat64(double left, double right, CompareOp op);
uint8_t CompareInt64Branchless(int64_t left, int64_t right, CompareOp op);
uint8_t CompareFloat64Branchless(double left, double right, CompareOp op);
bool PredicateCanMatchRange(double min, double max, CompareOp op, double value);

}  // namespace mimicdb

#endif  // MIMICDB_PREDICATE_H
