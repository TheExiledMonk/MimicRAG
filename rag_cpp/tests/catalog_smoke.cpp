#include "mimicrag/catalog.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

int main() {
    auto require = [](bool condition) { if (!condition) throw std::runtime_error("catalog smoke assertion failed"); };
    const auto path = std::filesystem::path("/tmp") / ("mimicrag_catalog_" + std::to_string(getpid()) + ".mrg");
    mimicrag::BinaryCatalog catalog(path);
    mimicrag::CatalogRecord first;
    first.document = {{"text", std::string(10000, 'a')}, {"source_uri", "test://one"}};
    first.local_embeddings = {{1.0F, 2.0F, 3.0F}, {4.0F, 5.0F, 6.0F}};
    catalog.Append(first);
    mimicrag::CatalogRecord second;
    second.document = {{"text", "second"}, {"source_uri", "test://two"}};
    second.remote_embeddings = {{0.25F, 0.5F}}; catalog.Append(second);
    size_t seen = 0;
    catalog.Replay([&](mimicrag::CatalogRecord&& row) {
        if (seen == 0) { require(row.document.at("source_uri") == "test://one"); require(row.local_embeddings == first.local_embeddings); }
        if (seen == 1) { require(row.document.at("source_uri") == "test://two"); require(row.remote_embeddings == second.remote_embeddings); }
        ++seen;
    });
    require(seen == 2); require(std::filesystem::file_size(path) < 1000);
    const auto valid_size = std::filesystem::file_size(path);
    { std::ofstream output(path, std::ios::binary | std::ios::app); output.write("RAG", 3); }
    seen = 0;
    seen = catalog.Replay([&](mimicrag::CatalogRecord&&) { ++seen; });
    require(seen == 2); require(std::filesystem::file_size(path) == valid_size);
    std::filesystem::remove(path); return 0;
}
