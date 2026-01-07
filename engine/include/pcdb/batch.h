#ifndef PCDB_BATCH_H
#define PCDB_BATCH_H

#include <cstddef>
#include <vector>

#include "pcdb/segment.h"

namespace pcdb {

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

}  // namespace pcdb

#endif  // PCDB_BATCH_H
