#include <cassert>

#include "mimicdb/hash.h"

int main() {
    const uint64_t hash_a = mimicdb::HashString(mimicdb::HashInit(), "schema");
    const uint64_t hash_b = mimicdb::HashString(mimicdb::HashInit(), "schema");
    const uint64_t hash_c = mimicdb::HashString(mimicdb::HashInit(), "schema2");
    assert(hash_a == hash_b);
    assert(hash_a != hash_c);
    return 0;
}
