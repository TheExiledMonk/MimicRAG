#include "mimicrag/document.h"
#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>

namespace mimicrag {
namespace {

std::string Lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string DetectFormat(const std::string& requested) {
    const auto format = Lower(requested);
    if (format.empty() || format == "auto" || format == "text" || format == "txt" || format == "plain") return "text";
    if (format == "md" || format == "markdown" || format == "text/markdown") return "markdown";
    if (format == "html" || format == "htm" || format == "text/html") return "html";
    throw std::runtime_error("unsupported document format: " + requested);
}

std::string JoinSections(const std::vector<std::string>& sections) {
    std::string out; for (const auto& section : sections) if (!section.empty()) { if (!out.empty()) out += " > "; out += section; } return out;
}

std::string MarkdownType(const std::string& line, bool code) {
    if (code) return "code";
    const std::string trimmed = Trim(line);
    if (std::regex_search(line, std::regex(R"(^\s{0,3}#{1,6}\s+)"))) return "heading";
    if (std::regex_search(line, std::regex(R"(^\s*(?:[-*+]\s+|\d+[.)]\s+))"))) return "list_item";
    if (std::regex_search(line, std::regex(R"(^\s*\|.*\|\s*$)"))) return "table_row";
    if (trimmed.rfind("[^", 0) == 0 && trimmed.find("]:") != std::string::npos) return "footnote";
    if (std::regex_search(line, std::regex(R"(!\[[^\]]*\]\([^)]+\))"))) return "caption";
    if (std::regex_search(line, std::regex(R"(\[[^\]]+\]\([^)]+\))"))) return "citation";
    return "paragraph";
}

void PushBlock(NormalizedDocument* document, std::string type, size_t start, size_t end,
               const std::string& section, size_t level = 0, std::string text = {}) {
    if (!document || end <= start) return;
    if (text.empty()) text = document->source_text.substr(start, end - start);
    if (Trim(text).empty()) return;
    document->blocks.push_back({std::move(type), std::move(text), section, start, end, 0, level});
}

NormalizedDocument ParseLines(const std::string& text, bool markdown, const std::string& title) {
    NormalizedDocument document; document.format = markdown ? "markdown" : "text"; document.title = title; document.source_text = text;
    std::vector<std::string> sections; bool code = false; size_t position = 0, paragraph_start = SIZE_MAX, paragraph_end = 0;
    auto flush = [&] { if (paragraph_start != SIZE_MAX) { PushBlock(&document, "paragraph", paragraph_start, paragraph_end, JoinSections(sections)); paragraph_start = SIZE_MAX; } };
    while (position < text.size()) {
        const size_t newline = text.find('\n', position), end = newline == std::string::npos ? text.size() : newline + 1;
        const std::string line = text.substr(position, end - position); const std::string trimmed = Trim(line);
        if (markdown && trimmed.rfind("```", 0) == 0) { flush();
            const size_t code_start = position; code = !code; size_t code_end = end;
            if (code) { size_t scan = end; while (scan < text.size()) { const size_t next = text.find('\n', scan), line_end = next == std::string::npos ? text.size() : next + 1;
                    if (Trim(text.substr(scan, line_end - scan)).rfind("```", 0) == 0) { code_end = line_end; code = false; break; } scan = line_end; }
                PushBlock(&document, "code", code_start, code_end, JoinSections(sections)); position = code_end; continue; }
        }
        const std::string type = markdown ? MarkdownType(line, code) : "paragraph";
        if (trimmed.empty()) flush();
        else if (markdown && type != "paragraph") {
            flush(); size_t level = 0;
            if (type == "heading") { while (level < trimmed.size() && trimmed[level] == '#') ++level; const std::string label = Trim(trimmed.substr(level));
                if (sections.size() >= level) sections.resize(level - 1);
                while (sections.size() + 1 < level) sections.push_back("");
                sections.push_back(label);
                if (document.title.empty() && level == 1) document.title = label; }
            PushBlock(&document, type, position, end, JoinSections(sections), level);
        } else { if (paragraph_start == SIZE_MAX) paragraph_start = position; paragraph_end = end; }
        if (newline == std::string::npos) { position = text.size(); break; } position = end;
    }
    flush(); return document;
}

std::string DecodeHtml(std::string value) {
    const std::vector<std::pair<std::string, std::string>> entities{{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}};
    for (const auto& [encoded, decoded] : entities) { size_t at = 0; while ((at = value.find(encoded, at)) != std::string::npos) { value.replace(at, encoded.size(), decoded); at += decoded.size(); } }
    return Trim(std::regex_replace(value, std::regex("<[^>]*>"), " "));
}

NormalizedDocument ParseHtml(const std::string& text, const std::string& supplied_title) {
    NormalizedDocument document; document.format = "html"; document.parser_version = "mimic-html-1"; document.title = supplied_title; document.source_text = text;
    const std::regex element(R"(<(h[1-6]|p|li|pre|code|table|tr|caption|figcaption|blockquote|aside|footer)[^>]*>[\s\S]*?</\1\s*>)", std::regex::icase);
    std::vector<std::string> sections;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), element); it != std::sregex_iterator(); ++it) {
        const size_t start = static_cast<size_t>(it->position()), end = start + static_cast<size_t>(it->length());
        std::string tag = Lower((*it)[1].str()), type = "paragraph"; size_t level = 0;
        if (tag[0] == 'h' && tag.size() == 2) { type = "heading"; level = static_cast<size_t>(tag[1] - '0'); }
        else if (tag == "li") type = "list_item"; else if (tag == "pre" || tag == "code") type = "code";
        else if (tag == "table" || tag == "tr") type = "table"; else if (tag == "caption" || tag == "figcaption") type = "caption";
        else if (tag == "footer") type = "footnote"; else if (tag == "blockquote") type = "citation";
        const std::string decoded = DecodeHtml(it->str());
        if (type == "heading") { if (sections.size() >= level) sections.resize(level - 1); while (sections.size() + 1 < level) sections.push_back(""); sections.push_back(decoded);
            if (document.title.empty() && level == 1) document.title = decoded; }
        PushBlock(&document, type, start, end, JoinSections(sections), level, decoded);
    }
    if (document.blocks.empty()) { auto fallback = ParseLines(DecodeHtml(text), false, supplied_title); fallback.format = "html"; fallback.parser_version = "mimic-html-1"; return fallback; }
    return document;
}

size_t BoundaryBefore(const std::string& text, size_t start, size_t desired, size_t minimum) {
    if (desired >= text.size()) return text.size();
    for (const char marker : {'\n', '.', ';', ' '}) { const size_t at = text.rfind(marker, desired); if (at != std::string::npos && at + 1 >= start + minimum) return at + 1; }
    return desired;
}
}  // namespace

NormalizedDocument ParseDocument(const std::string& text, const std::string& format, const std::string& title) {
    const auto selected = DetectFormat(format);
    if (selected == "markdown") return ParseLines(text, true, title);
    if (selected == "html") return ParseHtml(text, title);
    return ParseLines(text, false, title);
}

std::vector<ChunkPlan> PlanChunks(const NormalizedDocument& document, const ChunkingOptions& options) {
    if (options.target_chars < 80 || options.maximum_chars < options.target_chars || options.minimum_chars > options.target_chars || !options.maximum_chunks)
        throw std::runtime_error("invalid chunking resource budget");
    std::vector<ChunkPlan> chunks;
    if (options.mode == "fast") {
        for (size_t start = 0; start < document.source_text.size();) {
            size_t end = std::min(document.source_text.size(), start + options.target_chars);
            if (end < document.source_text.size()) { const size_t boundary = document.source_text.rfind(' ', end);
                if (boundary != std::string::npos && boundary > start + 80) end = boundary; }
            chunks.push_back({start, end, 0, 0, "", "fast-v1", ""}); if (end == document.source_text.size()) break;
            start = end > options.overlap_chars ? end - options.overlap_chars : end;
            if (chunks.size() >= options.maximum_chunks) throw std::runtime_error("document exceeds maximum chunk count");
        }
        return chunks;
    }
    if (options.mode != "structured" && options.mode != "semantic") throw std::runtime_error("ingestion mode must be fast, structured, or semantic");
    size_t chunk_start = SIZE_MAX, chunk_end = 0; std::string section;
    auto flush = [&] {
        if (chunk_start == SIZE_MAX) return;
        chunks.push_back({chunk_start, chunk_end, 0, 0, section, options.mode + "-adaptive-v1", ""}); chunk_start = SIZE_MAX;
        if (chunks.size() > options.maximum_chunks) throw std::runtime_error("document exceeds maximum chunk count");
    };
    for (const auto& block : document.blocks) {
        if (chunk_start != SIZE_MAX && (block.type == "heading" || block.end - chunk_start > options.target_chars)) flush();
        if (block.end - block.start > options.maximum_chars) {
            flush(); size_t start = block.start;
            while (start < block.end) { const size_t desired = std::min(block.end, start + options.maximum_chars);
                const size_t end = BoundaryBefore(document.source_text, start, desired, options.minimum_chars);
                chunks.push_back({start, end, block.page, block.page, block.section_path, options.mode + "-dense-v1", ""});
                if (chunks.size() > options.maximum_chunks) throw std::runtime_error("document exceeds maximum chunk count");
                if (end == block.end) break;
                start = end > options.overlap_chars ? end - options.overlap_chars : end; }
            continue;
        }
        if (chunk_start == SIZE_MAX) { chunk_start = block.start; section = block.section_path; }
        chunk_end = block.end;
        if (chunk_end - chunk_start >= options.target_chars || block.type == "table" || block.type == "code") flush();
    }
    flush();
    if (chunks.size() > 1 && chunks.back().end - chunks.back().start < options.minimum_chars) {
        chunks[chunks.size() - 2].end = chunks.back().end; chunks.pop_back();
    }
    return chunks;
}

nlohmann::json DocumentStructureJson(const NormalizedDocument& document) {
    nlohmann::json blocks = nlohmann::json::array();
    for (const auto& block : document.blocks) blocks.push_back({{"type", block.type}, {"start", block.start}, {"end", block.end},
        {"page", block.page}, {"section_path", block.section_path}, {"heading_level", block.heading_level}});
    return {{"format", document.format}, {"parser_version", document.parser_version}, {"title", document.title}, {"blocks", blocks}};
}
}  // namespace mimicrag
