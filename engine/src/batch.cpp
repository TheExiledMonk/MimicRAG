#include "mimicdb/batch.h"

namespace mimicdb {

BatchIterator::BatchIterator(const Segment& segment, size_t batch_size)
    : total_(segment.RowCount()), batch_size_(batch_size), cursor_(0) {}

bool BatchIterator::HasNext() const {
    return cursor_ < total_;
}

Batch BatchIterator::Next() {
    Batch batch;
    if (!HasNext() || batch_size_ == 0) {
        return batch;
    }
    batch.offset = cursor_;
    const size_t remaining = total_ - cursor_;
    batch.length = remaining < batch_size_ ? remaining : batch_size_;
    cursor_ += batch.length;
    return batch;
}

}  // namespace mimicdb
