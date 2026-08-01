#include "mimicrag/wiki_importer.h"
#include <bzlib.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <nlohmann/json.hpp>

namespace mimicrag {
namespace {

using Clock = std::chrono::steady_clock;

std::string Between(std::string_view value, std::string_view open, std::string_view close) {
    const size_t begin = value.find(open);
    if (begin == std::string_view::npos) return {};
    const size_t content = begin + open.size(), end = value.find(close, content);
    return end == std::string_view::npos ? std::string{} : std::string(value.substr(content, end - content));
}

std::string XmlDecode(std::string value) {
    const std::pair<std::string_view, std::string_view> entities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& [encoded, decoded] : entities) {
        size_t position = 0;
        while ((position = value.find(encoded, position)) != std::string::npos) {
            value.replace(position, encoded.size(), decoded); position += decoded.size();
        }
    }
    return value;
}

std::string UrlEncode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out; out.reserve(value.size() + 16);
    for (unsigned char byte : value) {
        if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~') out.push_back(static_cast<char>(byte));
        else if (byte == ' ') out.push_back('_');
        else { out.push_back('%'); out.push_back(hex[byte >> 4]); out.push_back(hex[byte & 15]); }
    }
    return out;
}

void StripBalanced(std::string* text, std::string_view open, std::string_view close) {
    size_t scan = 0;
    while ((scan = text->find(open, scan)) != std::string::npos) {
        const size_t begin = scan; size_t depth = 1; scan += open.size();
        while (scan < text->size() && depth) {
            const size_t nested = text->find(open, scan), end = text->find(close, scan);
            if (end == std::string::npos) { scan = begin + open.size(); break; }
            if (nested != std::string::npos && nested < end) { ++depth; scan = nested + open.size(); }
            else { --depth; scan = end + close.size(); }
        }
        if (!depth) { text->replace(begin, scan - begin, "\n"); scan = begin + 1; }
    }
}

std::string NormalizeWikitext(std::string text) {
    // Preserve section structure for MimicRAG's graph while cheaply removing the
    // most disruptive MediaWiki constructs. This is intentionally a streaming
    // importer, not a complete MediaWiki renderer.
    StripBalanced(&text, "{{", "}}");
    StripBalanced(&text, "{|", "|}");
    size_t position = 0;
    while (position < text.size()) {
        const size_t line_end = text.find('\n', position);
        const size_t end = line_end == std::string::npos ? text.size() : line_end;
        size_t left = position, right = end;
        while (left < right && text[left] == '=') ++left;
        while (right > left && text[right - 1] == '=') --right;
        const size_t level = left - position;
        if (level >= 2 && level <= 6 && end - right == level) {
            while (left < right && std::isspace(static_cast<unsigned char>(text[left]))) ++left;
            while (right > left && std::isspace(static_cast<unsigned char>(text[right - 1]))) --right;
            text.replace(position, end - position, std::string(level, '#') + " " + text.substr(left, right - left));
        }
        position = (text.find('\n', position) == std::string::npos) ? text.size() : text.find('\n', position) + 1;
    }
    // Strip comments and ref bodies; both are common sources of retrieval noise.
    const auto erase_pairs = [&](std::string_view open, std::string_view close) {
        size_t begin = 0;
        while ((begin = text.find(open, begin)) != std::string::npos) {
            const size_t end = text.find(close, begin + open.size());
            if (end == std::string::npos) break;
            text.erase(begin, end + close.size() - begin);
        }
    };
    erase_pairs("<!--", "-->"); erase_pairs("<ref", "</ref>"); erase_pairs("<gallery", "</gallery>");
    // Convert ordinary wiki links to their label (or target) without attempting
    // to expand templates, which requires a full MediaWiki runtime.
    position = 0;
    while ((position = text.find("[[", position)) != std::string::npos) {
        const size_t end = text.find("]]", position + 2); if (end == std::string::npos) break;
        std::string label = text.substr(position + 2, end - position - 2);
        const size_t colon = label.find(':');
        if (colon != std::string::npos) {
            std::string prefix = label.substr(0, colon);
            std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (prefix == "file" || prefix == "image" || prefix == "category") { text.replace(position, end + 2 - position, ""); continue; }
        }
        const size_t pipe = label.rfind('|'); if (pipe != std::string::npos) label.erase(0, pipe + 1);
        text.replace(position, end + 2 - position, label); position += label.size();
    }
    // Drop remaining HTML tags and formatting quotes while retaining their text.
    position = 0;
    while ((position = text.find('<', position)) != std::string::npos) {
        const size_t end = text.find('>', position + 1); if (end == std::string::npos) break;
        text.erase(position, end + 1 - position);
    }
    position = 0; while ((position = text.find("'''", position)) != std::string::npos) text.erase(position, 3);
    position = 0; while ((position = text.find("''", position)) != std::string::npos) text.erase(position, 2);
    // Collapse excessive blank lines introduced by removed templates and tables.
    position = 0;
    while ((position = text.find("\n\n\n", position)) != std::string::npos) text.erase(position, 1);
    return text;
}

struct Checkpoint { uint64_t page_id = 0; uint64_t imported = 0; };

Checkpoint ReadCheckpoint(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::ifstream input(path); if (!input) return {};
    try { nlohmann::json row; input >> row; return {row.value("page_id", 0ULL), row.value("imported", 0ULL)}; }
    catch (const std::exception&) { throw std::runtime_error("invalid Wikipedia checkpoint: " + path.string()); }
}

void WriteCheckpoint(const std::filesystem::path& path, uint64_t page_id, uint64_t imported) {
    if (path.empty()) return;
    std::filesystem::create_directories(path.parent_path().empty() ? "." : path.parent_path());
    const auto temporary = path.string() + ".tmp";
    { std::ofstream output(temporary, std::ios::trunc); if (!output) throw std::runtime_error("cannot write checkpoint: " + temporary);
      output << nlohmann::json{{"page_id", page_id}, {"imported", imported}}.dump(2) << '\n'; }
    std::filesystem::rename(temporary, path);
}

class Input {
public:
    explicit Input(const std::filesystem::path& path) : file_(std::fopen(path.c_str(), "rb")) {
        if (!file_) throw std::runtime_error("cannot open Wikipedia dump: " + path.string());
        compressed_ = path.extension() == ".bz2";
        if (compressed_) OpenStream(nullptr, 0);
    }
    ~Input() { if (bz_) { int error = BZ_OK; BZ2_bzReadClose(&error, bz_); } }
    size_t Read(char* output, size_t capacity) {
        if (!compressed_) return std::fread(output, 1, capacity, file_.get());
        if (stream_ended_) {
            stream_ended_ = false;
            if (unused_.empty() && std::feof(file_.get())) { finished_ = true; return 0; }
            OpenStream(unused_.empty() ? nullptr : unused_.data(), static_cast<int>(unused_.size()));
            unused_.clear();
        }
        int error = BZ_OK; const int count = BZ2_bzRead(&error, bz_, output, static_cast<int>(capacity));
        if (error != BZ_OK && error != BZ_STREAM_END) throw std::runtime_error("BZip2 decompression failed with code " + std::to_string(error));
        if (error == BZ_STREAM_END) {
            void* unused = nullptr; int unused_count = 0, close_error = BZ_OK;
            BZ2_bzReadGetUnused(&close_error, bz_, &unused, &unused_count);
            if (close_error != BZ_OK) throw std::runtime_error("cannot continue concatenated BZip2 stream");
            unused_.clear();
            if (unused_count > 0) { const char* bytes = static_cast<const char*>(unused); unused_.assign(bytes, bytes + unused_count); }
            BZ2_bzReadClose(&close_error, bz_); bz_ = nullptr; stream_ended_ = true;
        }
        return static_cast<size_t>(count);
    }
    bool Finished() const { return compressed_ ? finished_ : std::feof(file_.get()); }
private:
    void OpenStream(void* unused, int unused_count) {
        int error = BZ_OK; bz_ = BZ2_bzReadOpen(&error, file_.get(), 0, 0, unused, unused_count);
        if (error != BZ_OK) throw std::runtime_error("cannot initialize BZip2 reader");
    }
    struct Close { void operator()(std::FILE* file) const { if (file) std::fclose(file); } };
    std::unique_ptr<std::FILE, Close> file_;
    BZFILE* bz_ = nullptr;
    std::vector<char> unused_;
    bool compressed_ = false, finished_ = false, stream_ended_ = false;
};

}  // namespace

WikiImportStats ImportWikipedia(RagEngine* engine, const WikiImportOptions& options) {
    if (!options.dry_run && !engine) throw std::runtime_error("Wikipedia import requires a RAG engine");
    Input input(options.dump_path); WikiImportStats stats; const auto started = Clock::now();
    const Checkpoint checkpoint = options.resume ? ReadCheckpoint(options.checkpoint_path) : Checkpoint{};
    std::string buffer; buffer.reserve(2 * 1024 * 1024); std::vector<char> block(1024 * 1024);
    uint64_t last_page_id = checkpoint.page_id;
    auto report = [&] {
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        std::cerr << "wiki: pages=" << stats.pages_seen << " imported=" << stats.imported << " chunks=" << stats.chunks
                  << " rate=" << (seconds > 0 ? stats.imported / seconds : 0) << " docs/s\n";
    };
    while (!input.Finished() && (!options.limit || stats.imported < options.limit)) {
        const size_t count = input.Read(block.data(), block.size()); stats.source_bytes += count;
        buffer.append(block.data(), count);
        size_t consumed = 0;
        while (!options.limit || stats.imported < options.limit) {
            const size_t begin = buffer.find("<page>", consumed); if (begin == std::string::npos) break;
            const size_t end = buffer.find("</page>", begin + 6); if (end == std::string::npos) { consumed = begin; break; }
            const std::string_view page(buffer.data() + begin, end + 7 - begin); ++stats.pages_seen;
            const std::string ns = Between(page, "<ns>", "</ns>");
            std::string title = XmlDecode(Between(page, "<title>", "</title>"));
            const std::string id_text = Between(page, "<id>", "</id>");
            uint64_t page_id = 0; try { page_id = std::stoull(id_text); } catch (const std::exception&) {}
            if (ns == "0") {
                ++stats.main_namespace_pages;
                const bool redirect = page.find("<redirect ") != std::string_view::npos;
                if (redirect) ++stats.redirects_skipped;
                else if (checkpoint.page_id && page_id <= checkpoint.page_id) ++stats.resumed_skipped;
                else if (stats.main_namespace_pages <= options.skip) ++stats.resumed_skipped;
                else {
                    const size_t text_tag = page.find("<text");
                    const size_t text_begin = text_tag == std::string_view::npos ? std::string_view::npos : page.find('>', text_tag);
                    const size_t text_end = text_begin == std::string_view::npos ? std::string_view::npos : page.find("</text>", text_begin + 1);
                    if (text_end == std::string_view::npos || text_end == text_begin + 1) ++stats.empty_skipped;
                    else if (text_end - text_begin - 1 > options.max_article_bytes) ++stats.oversized_skipped;
                    else {
                        std::string text = NormalizeWikitext(XmlDecode(std::string(page.substr(text_begin + 1, text_end - text_begin - 1))));
                        if (text.find_first_not_of(" \t\r\n") == std::string::npos) ++stats.empty_skipped;
                        else {
                            if (!options.dry_run) {
                                auto result = engine->Ingest({{"text", std::move(text)}, {"source_uri", "https://en.wikipedia.org/wiki/" + UrlEncode(title)},
                                    {"document_id", "enwiki:" + std::to_string(page_id)}, {"tenant_id", options.tenant}, {"title", title}, {"access_scope", "public"},
                                    {"metadata", {{"corpus", "enwiki"}, {"page_id", page_id}, {"license", "CC BY-SA"}}}, {"background", false}});
                                stats.chunks += result.value("chunk_count", 0ULL);
                            }
                            ++stats.imported; last_page_id = page_id;
                            if (!options.dry_run && options.progress_every && stats.imported % options.progress_every == 0)
                                WriteCheckpoint(options.checkpoint_path, last_page_id, checkpoint.imported + stats.imported);
                            if (options.progress_every && stats.imported % options.progress_every == 0) report();
                        }
                    }
                }
            }
            consumed = end + 7;
        }
        if (consumed) buffer.erase(0, consumed);
        if (buffer.size() > options.max_article_bytes + 4 * 1024 * 1024) throw std::runtime_error("Wikipedia XML page exceeds parser buffer limit");
        if (count == 0 && !input.Finished()) throw std::runtime_error("Wikipedia dump read stalled");
    }
    if (!options.dry_run && stats.imported) WriteCheckpoint(options.checkpoint_path, last_page_id, checkpoint.imported + stats.imported);
    stats.elapsed_seconds = std::chrono::duration<double>(Clock::now() - started).count(); report(); return stats;
}

}  // namespace mimicrag
