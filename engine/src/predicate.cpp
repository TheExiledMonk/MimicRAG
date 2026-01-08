#include "mimicdb/predicate.h"

namespace mimicdb {

bool CompareInt64(int64_t left, int64_t right, CompareOp op) {
    switch (op) {
        case CompareOp::kEq:
            return left == right;
        case CompareOp::kNe:
            return left != right;
        case CompareOp::kLt:
            return left < right;
        case CompareOp::kLe:
            return left <= right;
        case CompareOp::kGt:
            return left > right;
        case CompareOp::kGe:
            return left >= right;
    }
    return false;
}

bool CompareFloat64(double left, double right, CompareOp op) {
    switch (op) {
        case CompareOp::kEq:
            return left == right;
        case CompareOp::kNe:
            return left != right;
        case CompareOp::kLt:
            return left < right;
        case CompareOp::kLe:
            return left <= right;
        case CompareOp::kGt:
            return left > right;
        case CompareOp::kGe:
            return left >= right;
    }
    return false;
}

uint8_t CompareInt64Branchless(int64_t left, int64_t right, CompareOp op) {
    const uint8_t eq = static_cast<uint8_t>(left == right);
    const uint8_t lt = static_cast<uint8_t>(left < right);
    const uint8_t gt = static_cast<uint8_t>(left > right);
    const uint8_t le = static_cast<uint8_t>(eq | lt);
    const uint8_t ge = static_cast<uint8_t>(eq | gt);
    const uint8_t ne = static_cast<uint8_t>(eq ^ 1U);
    const uint8_t is_eq = static_cast<uint8_t>(op == CompareOp::kEq);
    const uint8_t is_ne = static_cast<uint8_t>(op == CompareOp::kNe);
    const uint8_t is_lt = static_cast<uint8_t>(op == CompareOp::kLt);
    const uint8_t is_le = static_cast<uint8_t>(op == CompareOp::kLe);
    const uint8_t is_gt = static_cast<uint8_t>(op == CompareOp::kGt);
    const uint8_t is_ge = static_cast<uint8_t>(op == CompareOp::kGe);
    return static_cast<uint8_t>((eq & is_eq) | (ne & is_ne) | (lt & is_lt) | (le & is_le) |
                                (gt & is_gt) | (ge & is_ge));
}

uint8_t CompareFloat64Branchless(double left, double right, CompareOp op) {
    const uint8_t eq = static_cast<uint8_t>(left == right);
    const uint8_t lt = static_cast<uint8_t>(left < right);
    const uint8_t gt = static_cast<uint8_t>(left > right);
    const uint8_t le = static_cast<uint8_t>(eq | lt);
    const uint8_t ge = static_cast<uint8_t>(eq | gt);
    const uint8_t ne = static_cast<uint8_t>(eq ^ 1U);
    const uint8_t is_eq = static_cast<uint8_t>(op == CompareOp::kEq);
    const uint8_t is_ne = static_cast<uint8_t>(op == CompareOp::kNe);
    const uint8_t is_lt = static_cast<uint8_t>(op == CompareOp::kLt);
    const uint8_t is_le = static_cast<uint8_t>(op == CompareOp::kLe);
    const uint8_t is_gt = static_cast<uint8_t>(op == CompareOp::kGt);
    const uint8_t is_ge = static_cast<uint8_t>(op == CompareOp::kGe);
    return static_cast<uint8_t>((eq & is_eq) | (ne & is_ne) | (lt & is_lt) | (le & is_le) |
                                (gt & is_gt) | (ge & is_ge));
}

bool PredicateCanMatchRange(double min, double max, CompareOp op, double value) {
    switch (op) {
        case CompareOp::kEq:
            return value >= min && value <= max;
        case CompareOp::kNe:
            return min != max || value != min;
        case CompareOp::kLt:
            return min < value;
        case CompareOp::kLe:
            return min <= value;
        case CompareOp::kGt:
            return max > value;
        case CompareOp::kGe:
            return max >= value;
    }
    return true;
}

}  // namespace mimicdb
