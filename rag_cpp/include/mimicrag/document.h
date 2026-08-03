#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mimicrag {

struct DocumentBlock {
    std::string type;
    std::string text;
    std::string section_path;
    size_t start = 0;
    size_t end = 0;
    size_t page = 0;
    size_t heading_level = 0;
};

struct NormalizedDocument {
    std::string format;
    std::string parser_version = "mimic-structure-1";
    std::string title;
    std::string source_text;
    std::vector<DocumentBlock> blocks;
};

struct ChunkPlan {
    size_t start = 0;
    size_t end = 0;
    size_t page_start = 0;
    size_t page_end = 0;
    std::string section_path;
    std::string strategy;
    std::string contextual_header;
};

struct ChunkingOptions {
    std::string mode = "fast";
    size_t target_chars = 1600;
    size_t minimum_chars = 240;
    size_t maximum_chars = 2400;
    size_t overlap_chars = 160;
    size_t maximum_chunks = 10000;
};

NormalizedDocument ParseDocument(const std::string& text, const std::string& format,
                                 const std::string& title = "");
std::vector<ChunkPlan> PlanChunks(const NormalizedDocument& document,
                                  const ChunkingOptions& options);
nlohmann::json DocumentStructureJson(const NormalizedDocument& document);

}  // namespace mimicrag
