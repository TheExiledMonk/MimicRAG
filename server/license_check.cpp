#include "license_check.h"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <string_view>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace mimicdb {
namespace {

constexpr size_t kMaxLicenseBytes = 1024 * 1024;
constexpr int64_t kCheckIntervalSeconds = 24 * 60 * 60;
constexpr int64_t kOfflineGracePeriodSeconds = 7 * 24 * 60 * 60;

std::string Trim(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string ReadFirstFile(const char* const* paths, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        std::ifstream input(paths[i]);
        std::string value;
        if (input.is_open() && std::getline(input, value) && !Trim(value).empty()) {
            return Trim(value);
        }
    }
    return {};
}

size_t Collect(char* data, size_t size, size_t count, void* target) {
    auto* body = static_cast<std::string*>(target);
    const size_t bytes = size * count;
    if (bytes > kMaxLicenseBytes - body->size()) {
        return 0;  // Abort before accepting an unexpectedly large response.
    }
    body->append(data, bytes);
    return bytes;
}

bool Sha256Hex(std::string_view input, std::string* output) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        return false;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                    EVP_DigestUpdate(context, input.data(), input.size()) == 1 &&
                    EVP_DigestFinal_ex(context, digest.data(), &digest_size) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) {
        return false;
    }
    static constexpr char hex[] = "0123456789abcdef";
    output->clear();
    output->reserve(digest_size * 2);
    for (unsigned int i = 0; i < digest_size; ++i) {
        output->push_back(hex[digest[i] >> 4]);
        output->push_back(hex[digest[i] & 0x0f]);
    }
    return true;
}

}  // namespace

std::string LicenseMachineId() {
    std::string material;
#ifdef __APPLE__
    std::array<char, 256> platform_uuid{};
    size_t uuid_size = platform_uuid.size();
    if (::sysctlbyname("kern.uuid", platform_uuid.data(), &uuid_size, nullptr, 0) == 0) {
        material.assign(platform_uuid.data(), strnlen(platform_uuid.data(), uuid_size));
    }
#else
    constexpr const char* machine_id_paths[] = {
        "/etc/machine-id", "/var/lib/dbus/machine-id"};
    material = ReadFirstFile(machine_id_paths, 2);
#endif
    if (material.empty()) {
        std::array<char, 256> hostname{};
        if (::gethostname(hostname.data(), hostname.size() - 1) == 0) {
            material = hostname.data();
        }
    }
    if (material.empty()) {
        return {};
    }
    std::string result;
    if (!Sha256Hex("mimicdb-license-v1:" + material, &result)) {
        return {};
    }
    return result;
}

bool ReadLicenseState(const std::filesystem::path& path,
                      int64_t* last_success, int64_t* grace_start) {
    std::ifstream input(path);
    std::string value;
    if (!input.is_open()) {
        return false;
    }
    bool found = false;
    while (std::getline(input, value)) {
        const std::string line = Trim(value);
        const size_t separator = line.find('=');
        try {
            if (separator == std::string::npos) {
                // Accept the original one-line state format.
                *last_success = std::stoll(line);
                found = *last_success > 0;
            } else if (line.substr(0, separator) == "last_success") {
                *last_success = std::stoll(Trim(line.substr(separator + 1)));
                found = *last_success > 0;
            } else if (line.substr(0, separator) == "grace_start") {
                *grace_start = std::stoll(Trim(line.substr(separator + 1)));
                found = *grace_start > 0 || found;
            }
        } catch (...) {
            return false;
        }
    }
    return found;
}

void RecordLicenseState(const std::filesystem::path& path,
                        int64_t last_success, int64_t grace_start) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return;
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output.is_open()) {
            return;
        }
        output << "last_success=" << last_success << "\n"
               << "grace_start=" << grace_start << "\n";
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
    }
}

bool HasMachineLicense(const std::string& storage_root, std::string* reason) {
    const std::string machine_id = LicenseMachineId();
    if (machine_id.empty()) {
        *reason = "unable to determine a stable machine ID";
        return false;
    }

    const std::filesystem::path state_path =
        std::filesystem::path(storage_root) / ".mimicdb_license_last_success";
    int64_t last_success = 0;
    int64_t grace_start = 0;
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    if (ReadLicenseState(state_path, &last_success, &grace_start) &&
        last_success > 0 && now >= last_success &&
        now - last_success < kCheckIntervalSeconds) {
        *reason = "license was checked within the last 24 hours";
        return true;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        *reason = "libcurl initialization failed";
        return false;
    }
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, MIMICDB_LICENSE_URL);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mimicdb-license-check/1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        last_success = 0;
        grace_start = 0;
        ReadLicenseState(state_path, &last_success, &grace_start);
        int64_t grace_anchor = last_success > 0 ? last_success : grace_start;
        if (grace_anchor == 0) {
            grace_start = now;
            RecordLicenseState(state_path, 0, grace_start);
            grace_anchor = grace_start;
        }
        if (now >= grace_anchor && now - grace_anchor <= kOfflineGracePeriodSeconds) {
            *reason = "license service unreachable; within seven-day offline grace period";
            return true;
        }
        *reason = "license service unreachable and offline grace period expired";
        return false;
    }
    if (status < 200 || status >= 300) {
        *reason = "license file returned HTTP status " + std::to_string(status);
        return false;
    }

    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line)) {
        if (Trim(line) == machine_id) {
            const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            RecordLicenseState(state_path, now, 0);
            return true;
        }
    }
    *reason = "No valid license";
    return false;
}

}  // namespace mimicdb
