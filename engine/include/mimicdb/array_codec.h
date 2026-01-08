#ifndef MIMICDB_ARRAY_CODEC_H
#define MIMICDB_ARRAY_CODEC_H

#include <string>
#include <vector>

#include "mimicdb/dataset.h"

namespace mimicdb {

std::string EncodeArray(const std::vector<FieldValue>& values);
bool DecodeArray(const std::string& bytes, std::vector<FieldValue>* out);

}  // namespace mimicdb

#endif  // MIMICDB_ARRAY_CODEC_H
