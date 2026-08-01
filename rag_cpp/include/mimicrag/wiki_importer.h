#pragma once

#include "mimicrag/rag_engine.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace mimicrag {

struct WikiImportOptions {
    std::filesystem::path dump_path;
    std::filesystem::path checkpoint_path;
    std::string tenant = "wikipedia";
    size_t limit = 0;
    size_t skip = 0;
    size_t progress_every = 1000;
    size_t max_article_bytes = 8 * 1024 * 1024;
    bool resume = true;
    bool dry_run = false;
};

struct WikiImportStats {
    uint64_t pages_seen = 0;
    uint64_t main_namespace_pages = 0;
    uint64_t redirects_skipped = 0;
    uint64_t oversized_skipped = 0;
    uint64_t empty_skipped = 0;
    uint64_t resumed_skipped = 0;
    uint64_t imported = 0;
    uint64_t chunks = 0;
    uint64_t source_bytes = 0;
    double elapsed_seconds = 0;
};

WikiImportStats ImportWikipedia(RagEngine* engine, const WikiImportOptions& options);

}  // namespace mimicrag
