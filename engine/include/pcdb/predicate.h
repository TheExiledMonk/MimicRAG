#ifndef PCDB_PREDICATE_H
#define PCDB_PREDICATE_H

#include <cstddef>
#include <cstdint>

namespace pcdb {

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

}  // namespace pcdb

#endif  // PCDB_PREDICATE_H
