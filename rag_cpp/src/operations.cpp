#include "mimicrag/operations.h"
#include "mimicrag/catalog.h"
#include "mimicrag/local_embeddings.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace mimicrag {
namespace {
using json = nlohmann::json;

std::string Checksum(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary); if (!input) throw std::runtime_error("cannot read " + path.string());
    uint64_t hash = 1469598103934665603ULL; char buffer[1024 * 1024];
    while (input) { input.read(buffer, sizeof(buffer)); const auto count = input.gcount();
        for (std::streamsize i = 0; i < count; ++i) { hash ^= static_cast<unsigned char>(buffer[i]); hash *= 1099511628211ULL; } }
    std::ostringstream out; out << std::hex << std::setw(16) << std::setfill('0') << hash; return out.str();
}

json ReadManifest(const std::filesystem::path& snapshot) {
    std::ifstream input(snapshot / "manifest.json"); if (!input) throw std::runtime_error("snapshot manifest is missing");
    json manifest; input >> manifest;
    if (manifest.value("format", "") != "mimicrag-snapshot" || manifest.value("version", 0) != 1)
        throw std::runtime_error("unsupported snapshot manifest");
    return manifest;
}

void WriteJsonAtomic(const std::filesystem::path& path, const json& value) {
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    std::ofstream output(temporary, std::ios::trunc); output << value.dump(2) << '\n'; output.close();
    if (!output.good()) throw std::runtime_error("failed to write " + path.string());
    std::filesystem::rename(temporary, path);
}
}  // namespace

json CreateSnapshot(const Config& config, const std::filesystem::path& destination) {
    const std::filesystem::path source(config.server.data_path);
    if (!std::filesystem::exists(source / "catalog.mrg")) throw std::runtime_error("source catalog is missing");
    if (std::filesystem::exists(destination) && !std::filesystem::is_empty(destination)) throw std::runtime_error("snapshot destination must be empty");
    std::filesystem::create_directories(destination);
    json files = json::array();
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
        if (!entry.is_regular_file() || entry.path().extension() == ".tmp") continue;
        const auto name = entry.path().filename();
        std::filesystem::copy_file(entry.path(), destination / name, std::filesystem::copy_options::overwrite_existing);
        files.push_back({{"path", name.string()}, {"bytes", entry.file_size()}, {"checksum", Checksum(destination / name)}});
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    json manifest = {{"format", "mimicrag-snapshot"}, {"version", 1}, {"created_at_ms", now},
        {"catalog_format", "mrg1_zstd_float32"}, {"files", files}};
    WriteJsonAtomic(destination / "manifest.json", manifest);
    return {{"created", true}, {"destination", destination.string()}, {"files", files.size()}, {"verification", VerifySnapshot(destination)}};
}

json VerifySnapshot(const std::filesystem::path& snapshot) {
    const auto manifest = ReadManifest(snapshot); size_t checked = 0; uint64_t bytes = 0;
    for (const auto& file : manifest.at("files")) {
        const auto relative = std::filesystem::path(file.at("path").get<std::string>());
        if (relative.is_absolute() || relative.string().find("..") != std::string::npos) throw std::runtime_error("unsafe snapshot path");
        const auto path = snapshot / relative;
        if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) != file.at("bytes").get<uint64_t>() ||
            Checksum(path) != file.at("checksum").get<std::string>())
            throw std::runtime_error("snapshot verification failed for " + relative.string());
        ++checked; bytes += std::filesystem::file_size(path);
    }
    BinaryCatalog catalog(snapshot / "catalog.mrg"); const auto inspection = catalog.Inspect();
    return {{"valid", true}, {"files_checked", checked}, {"bytes_checked", bytes}, {"catalog", inspection}};
}

json RestoreSnapshot(const std::filesystem::path& snapshot, const std::filesystem::path& destination, bool rehearsal) {
    const auto verification = VerifySnapshot(snapshot);
    if (rehearsal) return {{"rehearsal", true}, {"would_restore_to", destination.string()}, {"verification", verification}};
    if (std::filesystem::exists(destination) && !std::filesystem::is_empty(destination)) throw std::runtime_error("restore destination must be empty");
    std::filesystem::create_directories(destination);
    const auto manifest = ReadManifest(snapshot);
    for (const auto& file : manifest.at("files")) {
        const auto relative = std::filesystem::path(file.at("path").get<std::string>());
        std::filesystem::copy_file(snapshot / relative, destination / relative, std::filesystem::copy_options::overwrite_existing);
    }
    std::filesystem::copy_file(snapshot / "manifest.json", destination / "manifest.json", std::filesystem::copy_options::overwrite_existing);
    return {{"restored", true}, {"destination", destination.string()}, {"source_verification", verification},
        {"restored_verification", VerifySnapshot(destination)}};
}

json Doctor(const Config& config) {
    json checks = json::array(); bool healthy = true;
    auto add = [&](const std::string& name, bool ok, const std::string& detail) { checks.push_back({{"name", name}, {"ok", ok}, {"detail", detail}}); healthy &= ok; };
    const std::filesystem::path data(config.server.data_path);
    std::error_code error; std::filesystem::create_directories(data, error);
    add("storage_directory", !error && std::filesystem::is_directory(data), error ? error.message() : data.string());
    const auto probe = data / ".doctor-write-probe"; { std::ofstream output(probe); output << "probe"; }
    const bool writable = std::filesystem::is_regular_file(probe); std::filesystem::remove(probe, error);
    add("storage_permissions", writable && !error, error ? error.message() : "readable and writable");
    if (std::filesystem::exists(data / "catalog.mrg")) {
        try { const auto inspected = BinaryCatalog(data / "catalog.mrg").Inspect(); add("catalog", inspected.value("valid", false), inspected.dump()); }
        catch (const std::exception& exception) { add("catalog", false, exception.what()); }
    } else add("catalog", true, "new instance");
    bool identity_key_available = false;
    for (const auto& key : config.server.keys) identity_key_available |= !key.key.empty() || (!key.key_env.empty() && std::getenv(key.key_env.c_str()));
    const bool auth_configured = identity_key_available || !ResolveServerKey(config.server).empty();
    add("server_auth", auth_configured, auth_configured ? "configured" : "no server key configured");
    if (config.local_embedding.enabled) add("local_model", std::filesystem::is_regular_file(config.local_embedding.model_path), config.local_embedding.model_path);
    else add("local_model", true, "disabled; deterministic BM25 fallback available");
    add("accelerator", true, config.local_embedding.enabled ? (config.local_embedding.gpu_layers == 0 ? "CPU forced" : "GPU requested with CPU fallback") : "not required");
    const auto space = std::filesystem::space(data, error);
    add("disk_capacity", !error && space.available > config.server.max_body_bytes * 2, error ? error.message() : std::to_string(space.available) + " bytes available");
    return {{"healthy", healthy}, {"checks", checks}};
}

json RepairDerivedIndexes(const Config& config) {
    const auto data = std::filesystem::path(config.server.data_path);
    const auto catalog = BinaryCatalog(data / "catalog.mrg").Inspect(); size_t removed = 0;
    for (const auto& name : {"content.dat", "content.manifest", "lexical.idx", "remote.ivf", "local.ivf"}) {
        std::error_code error; if (std::filesystem::remove(data / name, error)) ++removed;
        if (error) throw std::runtime_error("cannot remove derived index " + std::string(name) + ": " + error.message());
    }
    return {{"repair_scheduled", true}, {"removed_files", removed}, {"catalog", catalog}, {"rebuild_on_next_start", true}};
}
}  // namespace mimicrag
