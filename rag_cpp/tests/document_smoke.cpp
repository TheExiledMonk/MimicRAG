#include "mimicrag/document.h"
#include <stdexcept>

int main() {
    auto require = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(std::string("document smoke assertion failed: ") + message); };
    const std::string markdown =
        "# Manual\n\nIntro qualifier for the table below.\n\n"
        "| Name | Meaning |\n|---|---|\n| alpha | first |\n\n"
        "## Procedure\n\n- Keep headings attached.\n- Preserve list adjacency.\n\n"
        "```cpp\nint answer = 42;\n```\n\n[^1]: Authoritative footnote.\n";
    const auto parsed = mimicrag::ParseDocument(markdown, "markdown");
    require(parsed.title == "Manual", "markdown title"); require(parsed.parser_version == "mimic-structure-1", "parser version");
    bool heading = false, table = false, list = false, code = false, footnote = false;
    for (const auto& block : parsed.blocks) {
        require(block.end > block.start && block.end <= markdown.size(), "markdown offsets");
        heading |= block.type == "heading"; table |= block.type == "table_row"; list |= block.type == "list_item";
        code |= block.type == "code"; footnote |= block.type == "footnote";
    }
    if (!(heading && table && list && code && footnote)) throw std::runtime_error("markdown types h=" + std::to_string(heading) + " table=" + std::to_string(table) + " list=" + std::to_string(list) + " code=" + std::to_string(code) + " footnote=" + std::to_string(footnote));
    const auto chunks = mimicrag::PlanChunks(parsed, {"structured", 120, 40, 180, 20, 100});
    require(!chunks.empty(), "structured chunks");
    for (const auto& chunk : chunks) { require(chunk.end > chunk.start && chunk.end <= markdown.size(), "chunk offsets"); require(chunk.strategy.find("adaptive") != std::string::npos || chunk.strategy.find("dense") != std::string::npos, "chunk strategy"); }

    const std::string html = "<h1>Guide</h1><p>Qualified paragraph.</p><table><tr><td>A</td></tr></table><figcaption>Figure one</figcaption>";
    const auto html_document = mimicrag::ParseDocument(html, "html");
    require(html_document.title == "Guide", "html title"); require(html_document.format == "html", "html format"); require(html_document.blocks.size() >= 3, "html blocks");
    for (const auto& block : html_document.blocks) require(block.end <= html.size(), "html offsets");

    const auto fast = mimicrag::PlanChunks(mimicrag::ParseDocument(std::string(4000, 'x'), "text"), {"fast", 1600, 240, 2400, 200, 10});
    require(fast.size() == 3 && fast.front().start == 0 && fast.back().end == 4000, "fast chunks");
    return 0;
}
