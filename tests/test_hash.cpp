#include <cassert>

#include "pcdb/hash.h"

int main() {
    const uint64_t hash_a = pcdb::HashString(pcdb::HashInit(), "schema");
    const uint64_t hash_b = pcdb::HashString(pcdb::HashInit(), "schema");
    const uint64_t hash_c = pcdb::HashString(pcdb::HashInit(), "schema2");
    assert(hash_a == hash_b);
    assert(hash_a != hash_c);
    return 0;
}
