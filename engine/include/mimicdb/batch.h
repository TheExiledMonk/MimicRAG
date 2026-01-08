#ifndef MIMICDB_BATCH_H
#define MIMICDB_BATCH_H

#include <cstddef>
#include <vector>

#include "mimicdb/segment.h"

namespace mimicdb {

struct Batch {
    size_t offset = 0;
    size_t length = 0;
};

class BatchIterator {
public:
    BatchIterator(const Segment& segment, size_t batch_size);

    bool HasNext() const;
    Batch Next();

private:
    size_t total_ = 0;
    size_t batch_size_ = 0;
    size_t cursor_ = 0;
};

}  // namespace mimicdb

#endif  // MIMICDB_BATCH_H
