#include "mimicrag/config.h"
#include "mimicrag/http_server.h"
#include "mimicrag/rag_engine.h"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace {
std::string ReadFile(const std::string& path) { std::ifstream input(path, std::ios::binary); if (!input) throw std::runtime_error("cannot open: " + path); std::ostringstream out; out << input.rdbuf(); return out.str(); }
std::string Option(int argc, char** argv, const std::string& name, const std::string& fallback = "") { for (int i = 1; i + 1 < argc; ++i) if (argv[i] == name) return argv[i + 1]; return fallback; }
bool Flag(int argc, char** argv, const std::string& name) { for (int i = 1; i < argc; ++i) if (argv[i] == name) return true; return false; }
}

int main(int argc, char** argv) {
    try {
        std::string command = "serve";
        std::string config_path = Option(argc, argv, "--config", "");
        if (argc > 1 && std::string(argv[1]).rfind("--", 0) != 0) {
            const std::string first = argv[1];
            if (first == "serve" || first == "ingest" || first == "query" || first == "evaluate") command = first;
            else config_path = first;
        }
        if (config_path.empty()) config_path = "mimicrag.json";
        auto config = mimicrag::Config::Load(config_path);
        mimicrag::RagEngine engine(std::move(config));
        if (command == "ingest") {
            if (argc < 3) throw std::runtime_error("usage: mimicrag_server ingest PATH [--config FILE] [--tenant ID] [--source-uri URI] [--title TITLE]");
            const std::string path = argv[2];
            auto result = engine.Ingest({{"text", ReadFile(path)}, {"source_uri", Option(argc, argv, "--source-uri", "file://" + path)}, {"tenant_id", Option(argc, argv, "--tenant", "default")}, {"title", Option(argc, argv, "--title", "")}, {"background", false}});
            std::cout << result.dump(2) << '\n'; return 0;
        }
        if (command == "query") {
            if (argc < 3) throw std::runtime_error("usage: mimicrag_server query TEXT [--config FILE] [--tenant ID] [--scope SCOPE] [--top-k N]");
            nlohmann::json request = {{"query", argv[2]}, {"tenant_id", Option(argc, argv, "--tenant", "default")}, {"access_scope", Option(argc, argv, "--scope", "public")}};
            const auto top_k = Option(argc, argv, "--top-k"); if (!top_k.empty()) request["top_k"] = std::stoull(top_k);
            std::cout << engine.Answer(request).dump(2) << '\n'; return 0;
        }
        if (command == "evaluate") {
            if (argc < 3) throw std::runtime_error("usage: mimicrag_server evaluate GOLDEN_SET [--config FILE] [--generate] [--top-k N]");
            auto cases = nlohmann::json::parse(ReadFile(argv[2])); nlohmann::json request = {{"cases", cases}, {"generate", Flag(argc, argv, "--generate")}};
            const auto top_k = Option(argc, argv, "--top-k"); if (!top_k.empty()) request["top_k"] = std::stoull(top_k);
            std::cout << engine.Evaluate(request).dump(2) << '\n'; return 0;
        }
        mimicrag::HttpServer server(engine);
        server.Run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mimicrag_server: " << error.what() << '\n';
        return 1;
    }
}
