#include "mimicrag/catalog.h"
#include <zstd.h>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace mimicrag {
namespace {

constexpr std::array<char, 8> kFileMagic{'M', 'R', 'G', 'C', 'A', 'T', '1', '\0'};
constexpr uint32_t kRecordMagic = 0x31474152U;  // RAG1 in little endian.
constexpr uint32_t kVersion = 1;
constexpr uint32_t kRecordHeaderBytes = 48;
constexpr uint64_t kMaximumMetadataBytes = 64ULL * 1024 * 1024;
constexpr uint64_t kMaximumVectorBytes = 16ULL * 1024 * 1024 * 1024;

void RequireLittleEndian() {
    if constexpr (std::endian::native != std::endian::little) throw std::runtime_error("MimicRAG binary catalogs currently require a little-endian host");
}

template <typename T> void Write(std::ostream& output, T value) {
    static_assert(std::is_integral_v<T>); output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
template <typename T> bool Read(std::istream& input, T* value) {
    static_assert(std::is_integral_v<T>); return static_cast<bool>(input.read(reinterpret_cast<char*>(value), sizeof(*value)));
}

uint64_t HashBytes(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
    return hash;
}

struct Shape { uint32_t count = 0, dimension = 0; uint64_t bytes = 0; };
Shape Validate(const std::vector<std::vector<float>>& vectors) {
    Shape shape;
    if (vectors.empty()) return shape;
    if (vectors.size() > std::numeric_limits<uint32_t>::max() || vectors.front().size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("binary catalog vector shape is too large");
    shape.count = static_cast<uint32_t>(vectors.size()); shape.dimension = static_cast<uint32_t>(vectors.front().size());
    for (const auto& vector : vectors) if (vector.size() != shape.dimension) throw std::runtime_error("binary catalog contains ragged vectors");
    shape.bytes = static_cast<uint64_t>(shape.count) * shape.dimension * sizeof(float);
    if (shape.bytes > kMaximumVectorBytes) throw std::runtime_error("binary catalog vector block is too large");
    return shape;
}

void WriteVectors(std::ostream& output, const std::vector<std::vector<float>>& vectors) {
    for (const auto& vector : vectors) {
        const size_t bytes = vector.size() * sizeof(float);
        output.write(reinterpret_cast<const char*>(vector.data()), static_cast<std::streamsize>(bytes));
    }
}

std::vector<std::vector<float>> ReadVectors(std::istream& input, uint32_t count, uint32_t dimension, uint64_t* hash) {
    const uint64_t bytes = static_cast<uint64_t>(count) * dimension * sizeof(float);
    if (bytes > kMaximumVectorBytes) throw std::runtime_error("binary catalog vector block exceeds safety limit");
    std::vector<std::vector<float>> vectors(count, std::vector<float>(dimension));
    for (auto& vector : vectors) {
        const size_t vector_bytes = vector.size() * sizeof(float);
        if (!input.read(reinterpret_cast<char*>(vector.data()), static_cast<std::streamsize>(vector_bytes))) throw std::runtime_error("truncated binary catalog vector block");
        *hash = HashBytes(*hash, vector.data(), vector_bytes);
    }
    return vectors;
}

void Initialize(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path().empty() ? "." : path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create binary catalog: " + path.string());
    output.write(kFileMagic.data(), kFileMagic.size()); Write(output, kVersion); Write(output, uint32_t{0});
}

}  // namespace

BinaryCatalog::BinaryCatalog(std::filesystem::path path) : path_(std::move(path)) { RequireLittleEndian(); }
bool BinaryCatalog::Exists() const { return std::filesystem::exists(path_) && std::filesystem::file_size(path_) >= 16; }

void BinaryCatalog::Append(const CatalogRecord& record) {
    if (!Exists()) Initialize(path_);
    const std::string metadata = record.document.dump();
    if (metadata.size() > kMaximumMetadataBytes) throw std::runtime_error("binary catalog metadata exceeds safety limit");
    const size_t bound = ZSTD_compressBound(metadata.size()); std::vector<char> compressed(bound);
    const size_t compressed_size = ZSTD_compress(compressed.data(), compressed.size(), metadata.data(), metadata.size(), 3);
    if (ZSTD_isError(compressed_size)) throw std::runtime_error(std::string("Zstd catalog compression failed: ") + ZSTD_getErrorName(compressed_size));
    compressed.resize(compressed_size);
    const Shape remote = Validate(record.remote_embeddings), local = Validate(record.local_embeddings);
    uint64_t hash = HashBytes(1469598103934665603ULL, compressed.data(), compressed.size());
    for (const auto& vector : record.remote_embeddings) hash = HashBytes(hash, vector.data(), vector.size() * sizeof(float));
    for (const auto& vector : record.local_embeddings) hash = HashBytes(hash, vector.data(), vector.size() * sizeof(float));

    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error("cannot append binary catalog: " + path_.string());
    Write(output, kRecordMagic); Write(output, kRecordHeaderBytes);
    Write(output, static_cast<uint64_t>(compressed.size())); Write(output, static_cast<uint64_t>(metadata.size()));
    Write(output, remote.count); Write(output, remote.dimension); Write(output, local.count); Write(output, local.dimension); Write(output, hash);
    output.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
    WriteVectors(output, record.remote_embeddings); WriteVectors(output, record.local_embeddings);
    output.flush(); if (!output) throw std::runtime_error("failed to flush binary catalog record");
}

size_t BinaryCatalog::Replay(const std::function<void(CatalogRecord&&)>& consume) {
    if (!Exists()) return 0;
    std::ifstream input(path_, std::ios::binary); if (!input) throw std::runtime_error("cannot open binary catalog: " + path_.string());
    std::array<char, 8> magic{}; uint32_t version = 0, flags = 0;
    if (!input.read(magic.data(), magic.size()) || !Read(input, &version) || !Read(input, &flags) || magic != kFileMagic || version != kVersion)
        throw std::runtime_error("unsupported or corrupt binary catalog header: " + path_.string());
    (void)flags; size_t records = 0; uint64_t last_good = 16;
    const uint64_t file_size = std::filesystem::file_size(path_);
    for (;;) {
        const auto record_start = input.tellg(); uint32_t record_magic = 0, header_bytes = 0;
        if (!Read(input, &record_magic)) {
            if (input.eof() && static_cast<uint64_t>(record_start) == file_size) break;
            input.close(); std::filesystem::resize_file(path_, last_good); break;
        }
        uint64_t compressed_size = 0, metadata_size = 0, expected_hash = 0; uint32_t remote_count = 0, remote_dimension = 0, local_count = 0, local_dimension = 0;
        const bool header_ok = Read(input, &header_bytes) && Read(input, &compressed_size) && Read(input, &metadata_size) && Read(input, &remote_count) && Read(input, &remote_dimension)
            && Read(input, &local_count) && Read(input, &local_dimension) && Read(input, &expected_hash);
        if (!header_ok) { input.close(); std::filesystem::resize_file(path_, last_good); break; }
        if (record_magic != kRecordMagic || header_bytes != kRecordHeaderBytes || compressed_size > kMaximumMetadataBytes || metadata_size > kMaximumMetadataBytes)
            throw std::runtime_error("invalid binary catalog record header at byte " + std::to_string(static_cast<uint64_t>(record_start)));
        std::vector<char> compressed(compressed_size);
        if (!input.read(compressed.data(), static_cast<std::streamsize>(compressed.size()))) { input.close(); std::filesystem::resize_file(path_, last_good); break; }
        uint64_t hash = HashBytes(1469598103934665603ULL, compressed.data(), compressed.size());
        CatalogRecord record;
        try {
            record.remote_embeddings = ReadVectors(input, remote_count, remote_dimension, &hash);
            record.local_embeddings = ReadVectors(input, local_count, local_dimension, &hash);
        } catch (const std::runtime_error&) { input.close(); std::filesystem::resize_file(path_, last_good); break; }
        if (hash != expected_hash) throw std::runtime_error("binary catalog checksum mismatch at byte " + std::to_string(static_cast<uint64_t>(record_start)));
        std::string metadata(metadata_size, '\0');
        const size_t decoded = ZSTD_decompress(metadata.data(), metadata.size(), compressed.data(), compressed.size());
        if (ZSTD_isError(decoded) || decoded != metadata.size()) throw std::runtime_error("binary catalog metadata decompression failed");
        record.document = nlohmann::json::parse(metadata); consume(std::move(record)); ++records;
        last_good = static_cast<uint64_t>(input.tellg());
    }
    return records;
}

nlohmann::json BinaryCatalog::Inspect() {
    const uint64_t bytes = Exists() ? std::filesystem::file_size(path_) : 0;
    size_t records = 0, documents = 0, tombstones = 0;
    std::unordered_map<std::string, bool> live;
    records = Replay([&](CatalogRecord&& record) {
        const std::string id = record.document.value("document_id", "");
        if (id.empty()) throw std::runtime_error("catalog record has no document_id");
        if (record.document.value("_operation", "") == "delete") { live[id] = false; ++tombstones; }
        else { live[id] = true; ++documents; }
    });
    size_t live_documents = 0; for (const auto& item : live) live_documents += item.second;
    return {{"valid", true}, {"format", "mrg1_zstd_float32"}, {"format_version", 1}, {"bytes", bytes},
        {"records", records}, {"document_versions", documents}, {"tombstones", tombstones}, {"live_documents", live_documents}};
}

nlohmann::json BinaryCatalog::Compact() {
    if (!Exists()) return {{"compacted", false}, {"reason", "catalog_not_found"}};
    const uint64_t before = std::filesystem::file_size(path_);
    std::vector<CatalogRecord> records;
    std::unordered_map<std::string, size_t> latest;
    Replay([&](CatalogRecord&& record) {
        const std::string id = record.document.value("document_id", "");
        if (id.empty()) throw std::runtime_error("catalog record has no document_id");
        auto found = latest.find(id);
        if (found != latest.end()) records[found->second].document = nullptr;
        latest[id] = records.size(); records.push_back(std::move(record));
    });
    const auto temporary = std::filesystem::path(path_.string() + ".compacting");
    std::filesystem::remove(temporary);
    BinaryCatalog output(temporary); size_t retained = 0;
    for (const auto& record : records) {
        if (record.document.is_null() || record.document.value("_operation", "") == "delete") continue;
        output.Append(record); ++retained;
    }
    if (!output.Exists()) Initialize(temporary);
    // A same-filesystem rename is the generation switch. Keep the previous generation until the
    // replacement has been completely written and validated.
    BinaryCatalog validation(temporary); validation.Inspect();
    const auto previous = std::filesystem::path(path_.string() + ".previous");
    std::filesystem::remove(previous);
    std::filesystem::rename(path_, previous);
    try { std::filesystem::rename(temporary, path_); }
    catch (...) { std::filesystem::rename(previous, path_); throw; }
    std::filesystem::remove(previous);
    const uint64_t after = std::filesystem::file_size(path_);
    return {{"compacted", true}, {"records_before", records.size()}, {"records_after", retained},
        {"bytes_before", before}, {"bytes_after", after}, {"reclaimed_bytes", before > after ? before - after : 0}};
}

}  // namespace mimicrag
