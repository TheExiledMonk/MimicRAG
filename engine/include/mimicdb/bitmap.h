#ifndef MIMICDB_BITMAP_H
#define MIMICDB_BITMAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mimicdb {

class Bitmap {
public:
    Bitmap() = default;
    explicit Bitmap(size_t bits);

    void Resize(size_t bits);
    bool Get(size_t index) const;
    void Set(size_t index, bool value);
    size_t Size() const;
    const uint64_t* Words() const;
    size_t WordCount() const;
    void LoadWords(const uint64_t* words, size_t word_count, size_t bit_count);
    void AppendAllTrue(size_t count);
    void AppendBits(const uint8_t* validity, size_t count);

private:
    std::vector<uint64_t> words_;
    size_t bit_count_ = 0;
};

}  // namespace mimicdb

#endif  // MIMICDB_BITMAP_H
