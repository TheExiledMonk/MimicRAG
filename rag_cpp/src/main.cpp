#include "mimicrag/config.h"
#include "mimicrag/catalog.h"
#include "mimicrag/http_server.h"
#include "mimicrag/operations.h"
#include "mimicrag/rag_engine.h"
#include "mimicrag/wiki_importer.h"
#include <fstream>
#include <filesystem>
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
            if (first == "serve" || first == "ingest" || first == "delete" || first == "erase-tenant" || first == "retention" || first == "stats" || first == "inspect" || first == "compact" || first == "snapshot" || first == "verify-snapshot" || first == "restore" || first == "doctor" || first == "repair" || first == "migrate" || first == "rollback-migration" || first == "wiki-ingest" || first == "query" || first == "evaluate") command = first;
            else config_path = first;
        }
        if (config_path.empty()) config_path = "mimicrag.json";
        auto config = mimicrag::Config::Load(config_path);
        if (command == "snapshot") {
            if (argc < 3) throw std::runtime_error("usage: mimicrag_server snapshot DIRECTORY [--config FILE]");
            std::cout << mimicrag::CreateSnapshot(config, argv[2]).dump(2) << '\n'; return 0;
        }
        if (command == "verify-snapshot") {
            if (argc < 3) throw std::runtime_error("usage: mimicrag_server verify-snapshot DIRECTORY [--config FILE]");
            std::cout << mimicrag::VerifySnapshot(argv[2]).dump(2) << '\n'; return 0;
        }
        if (command == "restore") {
            if (argc < 3) throw std::runtime_error("usage: mimicrag_server restore SNAPSHOT [--destination DIRECTORY] [--rehearse] [--config FILE]");
            const auto destination = Option(argc, argv, "--destination", config.server.data_path);
            std::cout << mimicrag::RestoreSnapshot(argv[2], destination, Flag(argc, argv, "--rehearse")).dump(2) << '\n'; return 0;
        }
        if (command == "doctor") { const auto result = mimicrag::Doctor(config); std::cout << result.dump(2) << '\n'; return result.value("healthy", false) ? 0 : 2; }
        if (command == "repair") { std::cout << mimicrag::RepairDerivedIndexes(config).dump(2) << '\n'; return 0; }
        if (command == "rollback-migration") {
            const auto data = std::filesystem::path(config.server.data_path); const auto legacy = data / "catalog.jsonl"; const auto binary = data / "catalog.mrg";
            if (!std::filesystem::exists(legacy) || !std::filesystem::exists(binary)) throw std::runtime_error("rollback requires both legacy and binary catalogs");
            const auto rollback = data / "catalog.mrg.rolled_back"; if (std::filesystem::exists(rollback)) throw std::runtime_error("rollback artifact already exists");
            std::filesystem::rename(binary, rollback);
            for (const auto& name : {"content.dat", "content.manifest", "lexical.idx", "remote.ivf", "local.ivf"}) std::filesystem::remove(data / name);
            std::cout << nlohmann::json{{"rolled_back", true}, {"active_format", "legacy_jsonl"}, {"preserved_binary_catalog", rollback.string()}}.dump(2) << '\n'; return 0;
        }
        if (command == "inspect" || command == "compact") {
            mimicrag::BinaryCatalog catalog(std::filesystem::path(config.server.data_path) / "catalog.mrg");
            if (command == "inspect") { std::cout << catalog.Inspect().dump(2) << '\n'; return 0; }
            auto result = catalog.Compact();
            for (const auto& name : {"content.dat", "content.manifest", "lexical.idx", "remote.ivf", "local.ivf", "traces.jsonl"})
                std::filesystem::remove(std::filesystem::path(config.server.data_path) / name);
            result["derived_indexes_removed"] = true; result["rebuild_on_next_start"] = true;
            std::cout << result.dump(2) << '\n'; return 0;
        }
        mimicrag::RagEngine engine(config);
        if (command == "migrate") { std::cout << engine.Health().dump(2) << '\n'; return 0; }
        if (command == "serve" && engine.GetConfig().server.retention_days) engine.ApplyRetention(nlohmann::json::object());
        if (command == "ingest") {
            if (argc < 3) throw std::runtime_error("usage: mimicrag_server ingest PATH [--config FILE] [--tenant ID] [--source-uri URI] [--title TITLE] [--format text|markdown|html] [--mode fast|structured|semantic] [--background]");
            const std::string path = argv[2];
            std::string inferred_format = Option(argc, argv, "--format", "");
            if (inferred_format.empty()) { const auto extension = std::filesystem::path(path).extension().string(); inferred_format = extension == ".md" || extension == ".markdown" ? "markdown" : extension == ".html" || extension == ".htm" ? "html" : "text"; }
            auto result = engine.Ingest({{"text", ReadFile(path)}, {"source_uri", Option(argc, argv, "--source-uri", "file://" + path)}, {"tenant_id", Option(argc, argv, "--tenant", "default")}, {"title", Option(argc, argv, "--title", "")},
                {"format", inferred_format}, {"mode", Option(argc, argv, "--mode", config.ingestion.default_mode)}, {"background", Flag(argc, argv, "--background")}});
            std::cout << result.dump(2) << '\n'; return 0;
        }
        if (command == "delete") {
            if (argc < 3) throw std::runtime_error("usage: mimicrag_server delete DOCUMENT_ID [--config FILE] [--tenant ID]");
            std::cout << engine.DeleteDocument({{"document_id", argv[2]}, {"tenant_id", Option(argc, argv, "--tenant", "default")}}).dump(2) << '\n'; return 0;
        }
        if (command == "erase-tenant") {
            if (argc < 3) throw std::runtime_error("usage: mimicrag_server erase-tenant TENANT [--config FILE]");
            std::cout << engine.EraseTenant({{"tenant_id", argv[2]}}).dump(2) << '\n'; return 0;
        }
        if (command == "retention") {
            const auto days = Option(argc, argv, "--max-age-days");
            if (days.empty() && !config.server.retention_days) throw std::runtime_error("usage: mimicrag_server retention --max-age-days N [--tenant ID] [--config FILE]");
            nlohmann::json request = {{"max_age_days", days.empty() ? config.server.retention_days : std::stoull(days)}};
            const auto tenant = Option(argc, argv, "--tenant"); if (!tenant.empty()) request["tenant_id"] = tenant;
            std::cout << engine.ApplyRetention(request).dump(2) << '\n'; return 0;
        }
        if (command == "stats") { std::cout << engine.StorageStats().dump(2) << '\n'; return 0; }
        if (command == "wiki-ingest") {
            if (argc < 3) throw std::runtime_error("usage: mimicrag_server wiki-ingest DUMP.xml.bz2 [--config FILE] [--tenant ID] [--limit N] [--skip N] [--checkpoint FILE] [--progress N] [--dry-run] [--no-resume]");
            mimicrag::WikiImportOptions options; options.dump_path = argv[2]; options.tenant = Option(argc, argv, "--tenant", "wikipedia");
            options.checkpoint_path = Option(argc, argv, "--checkpoint", config.server.data_path + "/enwiki.checkpoint.json");
            const auto limit = Option(argc, argv, "--limit"); if (!limit.empty()) options.limit = std::stoull(limit);
            const auto skip = Option(argc, argv, "--skip"); if (!skip.empty()) options.skip = std::stoull(skip);
            const auto progress = Option(argc, argv, "--progress"); if (!progress.empty()) options.progress_every = std::stoull(progress);
            options.dry_run = Flag(argc, argv, "--dry-run"); options.resume = !Flag(argc, argv, "--no-resume");
            auto stats = mimicrag::ImportWikipedia(options.dry_run ? nullptr : &engine, options);
            std::cout << nlohmann::json{{"pages_seen", stats.pages_seen}, {"main_namespace_pages", stats.main_namespace_pages},
                {"redirects_skipped", stats.redirects_skipped}, {"oversized_skipped", stats.oversized_skipped}, {"empty_skipped", stats.empty_skipped},
                {"resumed_skipped", stats.resumed_skipped}, {"imported", stats.imported}, {"chunks", stats.chunks}, {"source_bytes", stats.source_bytes},
                {"elapsed_seconds", stats.elapsed_seconds}, {"documents_per_second", stats.elapsed_seconds > 0 ? stats.imported / stats.elapsed_seconds : 0}}.dump(2) << '\n'; return 0;
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
