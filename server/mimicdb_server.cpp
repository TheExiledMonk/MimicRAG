#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <csignal>
#include <openssl/evp.h>
#include <optional>
#include <string>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mimicdb/aggregate.h"
#include "mimicdb/compression.h"
#include "mimicdb/dataset.h"
#include "mimicdb/field_vector.h"
#include "mimicdb/segment_io.h"
#include "mimicdb/mask.h"
#include "mimicdb/predicate.h"
#include "mimicdb/scan.h"
#include "mimicdb/types.h"
#include "mimicdb/vector_search.h"

bool IsLocalBind(const std::string& host);
bool IsRootInitialized(const std::string& auth_db_path);

namespace mimicdb {

constexpr uint32_t kMagic = 0x4D434442;  // "MimicDB"
constexpr uint16_t kVersion = 1;
constexpr uint32_t kHostKeyMagic = 0x4D434B59;  // "MCKY"
constexpr uint16_t kHostKeyVersion = 2;
constexpr size_t kHostKeyBytes = 32;
constexpr size_t kEd25519PrivBytes = 32;
constexpr const char* kAuthDbName = "__auth__";
constexpr uint32_t kSecMagic = 0x4D534543;  // "MSEC"
constexpr uint16_t kSecVersion = 1;
constexpr uint16_t kCipherChaCha20Poly1305 = 1;
constexpr size_t kNonceBytes = 12;
constexpr size_t kSecNonceBytes = 32;
constexpr size_t kX25519Bytes = 32;
constexpr size_t kEd25519PubBytes = 32;
constexpr size_t kEd25519SigBytes = 64;
constexpr size_t kSecSessionIdBytes = 16;
constexpr uint16_t kFlagSessionId = 0x2;
constexpr uint8_t kKeyConfirmTag = 0xF1;

enum class OpCode : uint16_t {
    kPing = 1,
    kCreateDataset = 2,
    kAppendBatch = 3,
    kQueryAgg = 4,
    kHealth = 5,
    kCreateDatabase = 6,
    kListDatabases = 7,
    kScan = 8,
    kDropDatabase = 9,
    kDropDataset = 10,
    kHostKey = 11,
    kHostKeyRotate = 12,
    kVectorSearch = 13,
    kAuthInitRoot = 100,
    kAuthKeyAdd = 101,
    kAuthKeyDisable = 102,
    kAuthKeyRemove = 103,
    kAuthKeyList = 104,
    kAuthRoleCreate = 105,
    kAuthRoleDelete = 106,
    kAuthRoleGrant = 107,
    kAuthRoleRevoke = 108,
    kAuthAssignRole = 109,
    kAuthUnassignRole = 110,
    kAuthGrantKey = 111,
    kAuthRevokeKeyGrant = 112,
    kAuthRateLimitList = 113,
    kAuthRateLimitClear = 114,
    kAuthWhoami = 115,
};

enum class Status : uint16_t {
    kOk = 0,
    kBadRequest = 1,
    kNotFound = 2,
    kInternalError = 3,
    kUnsupported = 4,
    kAuthFailed = 5,
    kPermissionDenied = 6,
    kRateLimited = 7,
};

struct MessageHeader {
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    uint16_t flags = 0;
    uint16_t opcode = 0;
    uint16_t status = static_cast<uint16_t>(Status::kOk);
    uint32_t payload_size = 0;
    uint32_t request_id = 0;
};

struct HostKeyFileHeader {
    uint32_t magic = kHostKeyMagic;
    uint16_t version = kHostKeyVersion;
    uint16_t priv_len = static_cast<uint16_t>(kEd25519PrivBytes);
    uint16_t pub_len = static_cast<uint16_t>(kHostKeyBytes);
    uint16_t reserved = 0;
};

struct HostKeyFileHeaderV1 {
    uint32_t magic = kHostKeyMagic;
    uint16_t version = 1;
    uint16_t key_len = static_cast<uint16_t>(kHostKeyBytes);
};

bool IsAuthDatabaseName(const std::string& name) {
    return name == kAuthDbName;
}

std::string CombineScopes(const std::string& role_scope,
                          const std::string& assignment_scope) {
    if (assignment_scope.empty() || assignment_scope == "*") {
        return role_scope.empty() ? "*" : role_scope;
    }
    if (role_scope.empty() || role_scope == "*") {
        return assignment_scope;
    }
    if (role_scope == assignment_scope) {
        return role_scope;
    }
    return assignment_scope;
}

struct DatasetState {
    std::unique_ptr<Dataset> dataset;
    std::string path;
    size_t persisted_segments = 0;
    size_t segment_base_index = 0;
    uint64_t cached_bytes = 0;
    std::unordered_set<uint64_t> seen_batches;
};

struct DatabaseState {
    std::string name;
    std::string path;
    std::unordered_map<std::string, DatasetState> datasets;
};

struct SchemaFileHeader {
    uint32_t magic = 0x4D435343;  // "MCSC"
    uint32_t version = 1;
    uint16_t field_count = 0;
    uint16_t reserved = 0;
};

bool FsyncPath(const std::filesystem::path& path);

bool ReadExact(int fd, void* out, size_t size) {
    uint8_t* dst = static_cast<uint8_t*>(out);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t read_bytes = ::read(fd, dst + offset, size - offset);
        if (read_bytes == 0) {
            return false;
        }
        if (read_bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(read_bytes);
    }
    return true;
}

bool WriteExact(int fd, const void* data, size_t size) {
    const uint8_t* src = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd, src + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool WriteStream(int fd, const void* data, size_t size, size_t chunk_size) {
    const uint8_t* src = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset < size) {
        const size_t remaining = size - offset;
        const size_t to_write = remaining < chunk_size ? remaining : chunk_size;
        if (!WriteExact(fd, src + offset, to_write)) {
            return false;
        }
        offset += to_write;
    }
    return true;
}

struct Sha256State {
    uint64_t bitlen = 0;
    uint32_t state[8] = {
        0x6a09e667,
        0xbb67ae85,
        0x3c6ef372,
        0xa54ff53a,
        0x510e527f,
        0x9b05688c,
        0x1f83d9ab,
        0x5be0cd19,
    };
    uint8_t data[64] = {};
    size_t datalen = 0;
};

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t Sha256Ror(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32 - bits));
}

void Sha256Transform(Sha256State* ctx, const uint8_t data[64]) {
    uint32_t m[64];
    for (size_t i = 0; i < 16; ++i) {
        m[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
               (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(data[i * 4 + 3]));
    }
    for (size_t i = 16; i < 64; ++i) {
        const uint32_t s0 = Sha256Ror(m[i - 15], 7) ^
                            Sha256Ror(m[i - 15], 18) ^
                            (m[i - 15] >> 3);
        const uint32_t s1 = Sha256Ror(m[i - 2], 17) ^
                            Sha256Ror(m[i - 2], 19) ^
                            (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (size_t i = 0; i < 64; ++i) {
        const uint32_t s1 = Sha256Ror(e, 6) ^ Sha256Ror(e, 11) ^ Sha256Ror(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + s1 + ch + kSha256K[i] + m[i];
        const uint32_t s0 = Sha256Ror(a, 2) ^ Sha256Ror(a, 13) ^ Sha256Ror(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void Sha256Update(Sha256State* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            Sha256Transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void Sha256Final(Sha256State* ctx, uint8_t out[32]) {
    size_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) {
            ctx->data[i++] = 0x00;
        }
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) {
            ctx->data[i++] = 0x00;
        }
        Sha256Transform(ctx, ctx->data);
        std::memset(ctx->data, 0, 56);
    }

    ctx->bitlen += static_cast<uint64_t>(ctx->datalen) * 8;
    ctx->data[63] = static_cast<uint8_t>(ctx->bitlen);
    ctx->data[62] = static_cast<uint8_t>(ctx->bitlen >> 8);
    ctx->data[61] = static_cast<uint8_t>(ctx->bitlen >> 16);
    ctx->data[60] = static_cast<uint8_t>(ctx->bitlen >> 24);
    ctx->data[59] = static_cast<uint8_t>(ctx->bitlen >> 32);
    ctx->data[58] = static_cast<uint8_t>(ctx->bitlen >> 40);
    ctx->data[57] = static_cast<uint8_t>(ctx->bitlen >> 48);
    ctx->data[56] = static_cast<uint8_t>(ctx->bitlen >> 56);
    Sha256Transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>((ctx->state[0] >> (24 - i * 8)) & 0xff);
        out[i + 4] = static_cast<uint8_t>((ctx->state[1] >> (24 - i * 8)) & 0xff);
        out[i + 8] = static_cast<uint8_t>((ctx->state[2] >> (24 - i * 8)) & 0xff);
        out[i + 12] = static_cast<uint8_t>((ctx->state[3] >> (24 - i * 8)) & 0xff);
        out[i + 16] = static_cast<uint8_t>((ctx->state[4] >> (24 - i * 8)) & 0xff);
        out[i + 20] = static_cast<uint8_t>((ctx->state[5] >> (24 - i * 8)) & 0xff);
        out[i + 24] = static_cast<uint8_t>((ctx->state[6] >> (24 - i * 8)) & 0xff);
        out[i + 28] = static_cast<uint8_t>((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}

std::string HexEncode(const uint8_t* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = data[i];
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

std::string Sha256Hex(const uint8_t* data, size_t len) {
    Sha256State ctx;
    Sha256Update(&ctx, data, len);
    uint8_t hash[32] = {};
    Sha256Final(&ctx, hash);
    return HexEncode(hash, sizeof(hash));
}

std::string BytesToHex(const uint8_t* data, size_t len) {
    return HexEncode(data, len);
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streampos size = in.tellg();
    if (size < 0) {
        return false;
    }
    out->resize(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(out->data()), size)) {
        return false;
    }
    return true;
}

bool WriteFileBytes(const std::filesystem::path& path, const uint8_t* data, size_t len) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    return out.good();
}

bool ReadRandomBytes(uint8_t* out, size_t len) {
    const int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const bool ok = ReadExact(fd, out, len);
    ::close(fd);
    return ok;
}

bool GenerateEd25519Keypair(std::array<uint8_t, kEd25519PrivBytes>* out_priv,
                            std::array<uint8_t, kHostKeyBytes>* out_pub);

bool EnsureHostKey(const std::filesystem::path& path,
                   bool rotate,
                   std::array<uint8_t, kEd25519PrivBytes>* out_priv,
                   std::array<uint8_t, kHostKeyBytes>* out_pub,
                   std::string* out_fingerprint) {
    if (!rotate && std::filesystem::exists(path)) {
        std::vector<uint8_t> data;
        if (!ReadFileBytes(path, &data)) {
            return false;
        }
        if (data.size() >= sizeof(HostKeyFileHeader)) {
            HostKeyFileHeader header{};
            std::memcpy(&header, data.data(), sizeof(header));
            if (header.magic == kHostKeyMagic && header.version == kHostKeyVersion &&
                header.priv_len == kEd25519PrivBytes &&
                header.pub_len == kHostKeyBytes) {
                const size_t total = sizeof(header) + kEd25519PrivBytes + kHostKeyBytes;
                if (data.size() < total) {
                    return false;
                }
                std::memcpy(out_priv->data(), data.data() + sizeof(header),
                            kEd25519PrivBytes);
                std::memcpy(out_pub->data(),
                            data.data() + sizeof(header) + kEd25519PrivBytes,
                            kHostKeyBytes);
                *out_fingerprint = Sha256Hex(out_pub->data(), out_pub->size());
                return true;
            }
        }
        if (data.size() >= sizeof(HostKeyFileHeaderV1)) {
            HostKeyFileHeaderV1 legacy{};
            std::memcpy(&legacy, data.data(), sizeof(legacy));
            if (legacy.magic == kHostKeyMagic && legacy.version == 1 &&
                legacy.key_len == kHostKeyBytes) {
                rotate = true;
            }
        }
    }

    if (!GenerateEd25519Keypair(out_priv, out_pub)) {
        return false;
    }
    HostKeyFileHeader header{};
    std::vector<uint8_t> data(sizeof(header) + out_priv->size() + out_pub->size());
    std::memcpy(data.data(), &header, sizeof(header));
    std::memcpy(data.data() + sizeof(header), out_priv->data(), out_priv->size());
    std::memcpy(data.data() + sizeof(header) + out_priv->size(),
                out_pub->data(), out_pub->size());
    if (!WriteFileBytes(path, data.data(), data.size())) {
        return false;
    }
    *out_fingerprint = Sha256Hex(out_pub->data(), out_pub->size());
    return true;
}

void Sha256Digest(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256State ctx;
    Sha256Update(&ctx, data, len);
    Sha256Final(&ctx, out);
}

void HmacSha256(const uint8_t* key, size_t key_len,
                const uint8_t* data, size_t data_len,
                uint8_t out[32]) {
    uint8_t key_block[64] = {};
    if (key_len > sizeof(key_block)) {
        Sha256Digest(key, key_len, key_block);
        key_len = 32;
    }
    std::memcpy(key_block, key, key_len);
    uint8_t o_key[64];
    uint8_t i_key[64];
    for (size_t i = 0; i < sizeof(key_block); ++i) {
        o_key[i] = key_block[i] ^ 0x5c;
        i_key[i] = key_block[i] ^ 0x36;
    }
    Sha256State inner{};
    Sha256Update(&inner, i_key, sizeof(i_key));
    Sha256Update(&inner, data, data_len);
    uint8_t inner_hash[32];
    Sha256Final(&inner, inner_hash);
    Sha256State outer{};
    Sha256Update(&outer, o_key, sizeof(o_key));
    Sha256Update(&outer, inner_hash, sizeof(inner_hash));
    Sha256Final(&outer, out);
}

void HkdfSha256(const uint8_t* ikm, size_t ikm_len,
                const uint8_t* salt, size_t salt_len,
                const uint8_t* info, size_t info_len,
                uint8_t* out, size_t out_len) {
    uint8_t prk[32];
    HmacSha256(salt, salt_len, ikm, ikm_len, prk);
    uint8_t t[32];
    size_t t_len = 0;
    uint8_t counter = 1;
    size_t offset = 0;
    while (offset < out_len) {
        std::vector<uint8_t> input;
        input.reserve(t_len + info_len + 1);
        if (t_len > 0) {
            input.insert(input.end(), t, t + t_len);
        }
        if (info_len > 0) {
            input.insert(input.end(), info, info + info_len);
        }
        input.push_back(counter);
        HmacSha256(prk, sizeof(prk), input.data(), input.size(), t);
        t_len = sizeof(t);
        const size_t to_copy = std::min(out_len - offset, t_len);
        std::memcpy(out + offset, t, to_copy);
        offset += to_copy;
        counter += 1;
    }
}

struct SecureChannel {
    std::array<uint8_t, 32> key_c2s{};
    std::array<uint8_t, 32> key_s2c{};
    std::array<uint8_t, kSecSessionIdBytes> session_id{};
    uint64_t send_seq = 0;
    uint64_t recv_seq = 0;
};

struct PermissionSet {
    std::unordered_map<std::string, std::unordered_set<std::string>> grants;
};

struct SessionInfo {
    std::string session_id_hex;
    std::string fingerprint;
    PermissionSet permissions;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_activity;
};

struct ConnectionState {
    SecureChannel channel;
    std::string session_id;
};

struct RateLimitState {
    uint32_t fail_count = 0;
    std::chrono::system_clock::time_point next_allowed{};
    std::chrono::system_clock::time_point last_fail{};
    std::chrono::system_clock::time_point last_seen{};
};

bool AeadEncrypt(const std::array<uint8_t, 32>& key,
                 uint64_t seq,
                 const uint8_t* plaintext,
                 size_t plaintext_len,
                 std::vector<uint8_t>* out) {
    uint8_t nonce[kNonceBytes] = {};
    std::memcpy(nonce + 4, &seq, sizeof(seq));
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    int ok = EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                                   static_cast<int>(sizeof(nonce)), nullptr);
    ok = ok && EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce);
    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    out->resize(plaintext_len + 16);
    int out_len = 0;
    int total = 0;
    if (!EVP_EncryptUpdate(ctx, out->data(), &out_len, plaintext,
                           static_cast<int>(plaintext_len))) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    total += out_len;
    if (!EVP_EncryptFinal_ex(ctx, out->data() + total, &out_len)) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    total += out_len;
    uint8_t tag[16];
    if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, sizeof(tag), tag)) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    out->resize(total + sizeof(tag));
    std::memcpy(out->data() + total, tag, sizeof(tag));
    EVP_CIPHER_CTX_free(ctx);
    return true;
}

bool AeadDecrypt(const std::array<uint8_t, 32>& key,
                 uint64_t seq,
                 const uint8_t* ciphertext,
                 size_t ciphertext_len,
                 std::vector<uint8_t>* out) {
    if (ciphertext_len < 16) {
        return false;
    }
    const size_t data_len = ciphertext_len - 16;
    const uint8_t* tag = ciphertext + data_len;
    uint8_t nonce[kNonceBytes] = {};
    std::memcpy(nonce + 4, &seq, sizeof(seq));
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    int ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                                   static_cast<int>(sizeof(nonce)), nullptr);
    ok = ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce);
    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    out->resize(data_len);
    int out_len = 0;
    int total = 0;
    if (!EVP_DecryptUpdate(ctx, out->data(), &out_len, ciphertext,
                           static_cast<int>(data_len))) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    total += out_len;
    if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                             const_cast<uint8_t*>(tag))) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    if (EVP_DecryptFinal_ex(ctx, out->data() + total, &out_len) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    total += out_len;
    out->resize(total);
    EVP_CIPHER_CTX_free(ctx);
    return true;
}

bool GenerateX25519Key(EVP_PKEY** out_key, std::array<uint8_t, kX25519Bytes>* out_pub) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) {
        return false;
    }
    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen(ctx, &key) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    size_t len = out_pub->size();
    if (EVP_PKEY_get_raw_public_key(key, out_pub->data(), &len) <= 0 ||
        len != out_pub->size()) {
        EVP_PKEY_free(key);
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);
    *out_key = key;
    return true;
}

bool DeriveX25519Secret(EVP_PKEY* priv_key,
                        const uint8_t* peer_pub,
                        size_t peer_len,
                        std::array<uint8_t, kX25519Bytes>* out_shared) {
    EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                 peer_pub, peer_len);
    if (!peer) {
        return false;
    }
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(priv_key, nullptr);
    if (!ctx) {
        EVP_PKEY_free(peer);
        return false;
    }
    if (EVP_PKEY_derive_init(ctx) <= 0 ||
        EVP_PKEY_derive_set_peer(ctx, peer) <= 0) {
        EVP_PKEY_free(peer);
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    size_t out_len = out_shared->size();
    if (EVP_PKEY_derive(ctx, out_shared->data(), &out_len) <= 0 ||
        out_len != out_shared->size()) {
        EVP_PKEY_free(peer);
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    EVP_PKEY_free(peer);
    EVP_PKEY_CTX_free(ctx);
    return true;
}

bool GenerateEd25519Keypair(std::array<uint8_t, kEd25519PrivBytes>* out_priv,
                            std::array<uint8_t, kHostKeyBytes>* out_pub) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) {
        return false;
    }
    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen(ctx, &key) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    size_t priv_len = out_priv->size();
    size_t pub_len = out_pub->size();
    if (EVP_PKEY_get_raw_private_key(key, out_priv->data(), &priv_len) <= 0 ||
        priv_len != out_priv->size() ||
        EVP_PKEY_get_raw_public_key(key, out_pub->data(), &pub_len) <= 0 ||
        pub_len != out_pub->size()) {
        EVP_PKEY_free(key);
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(ctx);
    return true;
}

bool SignEd25519(const uint8_t* priv_key, size_t priv_len,
                 const uint8_t* message, size_t message_len,
                 std::array<uint8_t, kEd25519SigBytes>* out_sig) {
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                 priv_key, priv_len);
    if (!key) {
        return false;
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(key);
        return false;
    }
    if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) <= 0) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(key);
        return false;
    }
    size_t sig_len = out_sig->size();
    if (EVP_DigestSign(ctx, out_sig->data(), &sig_len, message, message_len) <= 0 ||
        sig_len != out_sig->size()) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(key);
        return false;
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return true;
}

bool VerifyEd25519(const uint8_t* pub_key, size_t pub_len,
                   const uint8_t* message, size_t message_len,
                   const uint8_t* signature, size_t sig_len) {
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                pub_key, pub_len);
    if (!key) {
        return false;
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(key);
        return false;
    }
    if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) <= 0) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(key);
        return false;
    }
    const int ok = EVP_DigestVerify(ctx, signature, sig_len, message, message_len);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok == 1;
}

template <typename T>
bool ReadScalar(const std::vector<uint8_t>& payload, size_t* cursor, T* out) {
    if (*cursor + sizeof(T) > payload.size()) {
        return false;
    }
    std::memcpy(out, payload.data() + *cursor, sizeof(T));
    *cursor += sizeof(T);
    return true;
}

bool ReadBytes(const std::vector<uint8_t>& payload, size_t* cursor, size_t len,
               std::string* out) {
    if (*cursor + len > payload.size()) {
        return false;
    }
    out->assign(reinterpret_cast<const char*>(payload.data() + *cursor), len);
    *cursor += len;
    return true;
}

bool ReadName(const std::vector<uint8_t>& payload, size_t* cursor, std::string* out) {
    uint16_t name_len = 0;
    if (!ReadScalar(payload, cursor, &name_len)) {
        return false;
    }
    return ReadBytes(payload, cursor, name_len, out);
}

bool ReadPredicateCount(const std::vector<uint8_t>& payload, size_t* cursor, uint16_t* out) {
    if (*cursor == payload.size()) {
        *out = 0;
        return true;
    }
    return ReadScalar(payload, cursor, out);
}

size_t TypeSize(FieldType type) {
    switch (type) {
        case FieldType::kInt32:
            return sizeof(int32_t);
        case FieldType::kInt64:
            return sizeof(int64_t);
        case FieldType::kFloat64:
            return sizeof(double);
        case FieldType::kBool:
            return sizeof(uint8_t);
        case FieldType::kDictInt32:
            return sizeof(int32_t);
        case FieldType::kString:
        case FieldType::kBytes:
            return 0;
        case FieldType::kArray:
        case FieldType::kObject:
        case FieldType::kVectorFloat32:
            return 0;
    }
    return 0;
}

FieldType DecodeFieldType(uint8_t type) {
    return static_cast<FieldType>(type);
}

uint8_t EncodeFieldType(FieldType type) {
    switch (type) {
        case FieldType::kInt32:
            return 0;
        case FieldType::kInt64:
            return 1;
        case FieldType::kFloat64:
            return 2;
        case FieldType::kBool:
            return 3;
        case FieldType::kDictInt32:
            return 4;
        case FieldType::kString:
            return 5;
        case FieldType::kBytes:
            return 6;
        case FieldType::kArray:
            return 7;
        case FieldType::kObject:
            return 8;
        case FieldType::kVectorFloat32:
            return 9;
    }
    return 0;
}

CompareOp DecodeCompareOp(uint8_t value) {
    switch (value) {
        case 0:
            return CompareOp::kEq;
        case 1:
            return CompareOp::kNe;
        case 2:
            return CompareOp::kLt;
        case 3:
            return CompareOp::kLe;
        case 4:
            return CompareOp::kGt;
        case 5:
            return CompareOp::kGe;
        default:
            return CompareOp::kEq;
    }
}

struct PredicateSpec {
    uint16_t field_index = 0;
    CompareOp op = CompareOp::kEq;
    double value = 0.0;
};

bool EvaluatePredicate(const FieldVector& field, CompareOp op, double value, size_t index) {
    if (!field.IsValid(index)) {
        return false;
    }
    double field_value = 0.0;
    switch (field.Type()) {
        case FieldType::kInt32:
            field_value = static_cast<double>(field.DataInt32()[index]);
            break;
        case FieldType::kInt64:
            field_value = static_cast<double>(field.DataInt64()[index]);
            break;
        case FieldType::kFloat64:
            field_value = field.DataFloat64()[index];
            break;
        case FieldType::kBool:
            field_value = field.DataBool()[index] ? 1.0 : 0.0;
            break;
        case FieldType::kDictInt32: {
            const uint32_t id = field.DataDictIds()[index];
            field_value = static_cast<double>(field.DictionaryValue(id));
            break;
        }
        case FieldType::kString:
        case FieldType::kBytes:
        case FieldType::kArray:
        case FieldType::kObject:
            return false;
    }
    switch (op) {
        case CompareOp::kEq:
            return field_value == value;
        case CompareOp::kNe:
            return field_value != value;
        case CompareOp::kLt:
            return field_value < value;
        case CompareOp::kLe:
            return field_value <= value;
        case CompareOp::kGt:
            return field_value > value;
        case CompareOp::kGe:
            return field_value >= value;
    }
    return false;
}

bool BuildPredicateMask(const std::vector<FieldVector>& fields,
                        const std::vector<PredicateSpec>& predicates,
                        Mask* out) {
    if (!out) {
        return false;
    }
    if (fields.empty()) {
        out->Resize(0);
        return true;
    }
    const size_t count = fields.front().Size();
    out->Resize(count);
    for (size_t i = 0; i < count; ++i) {
        out->Set(i, true);
    }
    for (const auto& pred : predicates) {
        if (pred.field_index >= fields.size()) {
            return false;
        }
        const auto& field = fields[pred.field_index];
        for (size_t i = 0; i < count; ++i) {
            if (!out->Get(i)) {
                continue;
            }
            if (!EvaluatePredicate(field, pred.op, pred.value, i)) {
                out->Set(i, false);
            }
        }
    }
    return true;
}
void MergeAggregate(const AggregateResult& src, AggregateResult* dst) {
    if (!dst->has_value && src.has_value) {
        dst->min = src.min;
        dst->max = src.max;
        dst->has_value = true;
    } else if (src.has_value) {
        if (src.min < dst->min) {
            dst->min = src.min;
        }
        if (src.max > dst->max) {
            dst->max = src.max;
        }
    }
    dst->count += src.count;
    dst->sum += src.sum;
}

class Server {
public:
    Server(std::string bind_addr, uint16_t port, std::string storage_root,
           std::string auth_db_path, std::string host_key_path,
           std::array<uint8_t, kEd25519PrivBytes> host_key_priv,
           std::vector<uint8_t> host_key_pub, std::string host_key_fingerprint,
           uint32_t session_idle_timeout_sec, uint32_t session_max_lifetime_sec,
           size_t session_max_total, size_t session_max_per_fingerprint,
           uint32_t handshake_nonce_ttl_sec, size_t handshake_nonce_max_entries,
           int auth_failure_delay_ms,
           uint32_t auth_rate_limit_burst, std::vector<uint32_t> auth_rate_limit_backoff_sec,
           size_t auth_rate_limit_state_max_entries, uint32_t auth_rate_limit_state_ttl_sec,
           size_t auth_rate_limit_max_rows, size_t auth_audit_log_max_rows,
           size_t auth_prune_batch_rows,
           bool flush_on_shutdown, bool flush_on_seal, int flush_interval_ms,
           uint32_t max_payload_bytes, uint32_t max_rows_per_batch, int append_sleep_ms,
           size_t segment_cache_max, uint64_t segment_cache_bytes, size_t query_threads,
           bool compression_metrics)
        : bind_addr_(std::move(bind_addr)),
          port_(port),
          storage_root_(std::move(storage_root)),
          auth_db_path_(std::move(auth_db_path)),
          host_key_path_(std::move(host_key_path)),
          host_key_priv_(host_key_priv),
          host_key_pub_(std::move(host_key_pub)),
          host_key_fingerprint_(std::move(host_key_fingerprint)),
          session_idle_timeout_sec_(session_idle_timeout_sec),
          session_max_lifetime_sec_(session_max_lifetime_sec),
          session_max_total_(session_max_total),
          session_max_per_fingerprint_(session_max_per_fingerprint),
          handshake_nonce_ttl_sec_(handshake_nonce_ttl_sec),
          handshake_nonce_max_entries_(handshake_nonce_max_entries),
          auth_failure_delay_ms_(auth_failure_delay_ms),
          auth_rate_limit_burst_(auth_rate_limit_burst),
          auth_rate_limit_backoff_sec_(std::move(auth_rate_limit_backoff_sec)),
          auth_rate_limit_state_max_entries_(auth_rate_limit_state_max_entries),
          auth_rate_limit_state_ttl_sec_(auth_rate_limit_state_ttl_sec),
          auth_rate_limit_max_rows_(auth_rate_limit_max_rows),
          auth_audit_log_max_rows_(auth_audit_log_max_rows),
          auth_prune_batch_rows_(auth_prune_batch_rows),
          flush_on_shutdown_(flush_on_shutdown),
          flush_on_seal_(flush_on_seal),
          flush_interval_ms_(flush_interval_ms),
          max_payload_bytes_(max_payload_bytes),
          max_rows_per_batch_(max_rows_per_batch),
          append_sleep_ms_(append_sleep_ms),
          segment_cache_max_(segment_cache_max),
          segment_cache_bytes_(segment_cache_bytes),
          query_threads_(query_threads),
          compression_metrics_(compression_metrics) {}

    int Run() {
        running_.store(true);
        instance_ = this;
        std::signal(SIGINT, &HandleSignal);
        std::signal(SIGTERM, &HandleSignal);
        RecoverDatasets();
        EnsureDatabase("default");
        EnsureAuthDatabase();
        LoadRateLimitState();
        PruneRateLimitState();
        StartHousekeeping();
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            std::cerr << "socket failed\n";
            return 1;
        }
        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        if (bind_addr_.empty() || bind_addr_ == "0.0.0.0") {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr) != 1) {
            std::cerr << "invalid bind address, falling back to 0.0.0.0\n";
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "bind failed\n";
            ::close(fd);
            return 1;
        }
        if (::listen(fd, 8) < 0) {
            std::cerr << "listen failed\n";
            ::close(fd);
            return 1;
        }
        std::cout << "mimicdb_server listening on " << port_ << "\n";
        while (running_.load()) {
            const int client = ::accept(fd, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::cerr << "accept failed\n";
                break;
            }
            active_clients_.fetch_add(1);
            std::thread([this, client]() {
                HandleClient(client);
                ::close(client);
                active_clients_.fetch_sub(1);
            }).detach();
        }
        while (active_clients_.load() != 0) {
            ::usleep(1000);
        }
        if (flush_on_shutdown_) {
            FlushActiveSegments();
        }
        StopHousekeeping();
        ::close(fd);
        return 0;
    }

private:
    static void HandleSignal(int) {
        if (instance_ != nullptr) {
            instance_->running_.store(false);
        }
    }

    bool ReadSecureFrame(int client,
                         const std::array<uint8_t, 32>& key,
                         uint64_t expected_seq,
                         std::vector<uint8_t>* out_plaintext) {
        uint8_t header[12];
        if (!ReadExact(client, header, sizeof(header))) {
            return false;
        }
        uint64_t seq = 0;
        uint32_t len = 0;
        std::memcpy(&seq, header, sizeof(seq));
        std::memcpy(&len, header + sizeof(seq), sizeof(len));
        if (seq != expected_seq) {
            return false;
        }
        const uint64_t max_frame =
            sizeof(MessageHeader) + max_payload_bytes_ + 16ULL;
        if (len == 0 || len > max_frame) {
            return false;
        }
        std::vector<uint8_t> ciphertext(len);
        if (!ReadExact(client, ciphertext.data(), ciphertext.size())) {
            return false;
        }
        if (!AeadDecrypt(key, seq, ciphertext.data(), ciphertext.size(), out_plaintext)) {
            return false;
        }
        return true;
    }

    std::string GetRemoteAddr(int client) const {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (getpeername(client, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return "unknown";
        }
        char buf[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf))) {
            return "unknown";
        }
        return std::string(buf);
    }

    std::string RateLimitKey(const std::string& remote,
                             const std::string& fingerprint) const {
        return remote + "|" + fingerprint;
    }

    int64_t RateLimitWaitSeconds(const std::string& remote,
                                 const std::string& fingerprint) {
        const auto now = std::chrono::system_clock::now();
        auto compute_wait = [&](const std::string& key) -> int64_t {
            auto it = rate_limit_state_.find(key);
            if (it == rate_limit_state_.end()) {
                return 0;
            }
            it->second.last_seen = now;
            if (now >= it->second.next_allowed) {
                return 0;
            }
            const auto wait =
                std::chrono::duration_cast<std::chrono::seconds>(
                    it->second.next_allowed - now).count();
            return wait < 0 ? 0 : wait;
        };
        const int64_t wait_remote = compute_wait(RateLimitKey(remote, ""));
        const int64_t wait_fp = compute_wait(RateLimitKey(remote, fingerprint));
        return std::max(wait_remote, wait_fp);
    }

    void ResetRateLimit(const std::string& remote, const std::string& fingerprint) {
        rate_limit_state_.erase(RateLimitKey(remote, fingerprint));
        rate_limit_state_.erase(RateLimitKey(remote, ""));
    }

    void RecordRateLimitFailure(const std::string& remote, const std::string& fingerprint) {
        const auto now = std::chrono::system_clock::now();
        auto update_entry = [&](const std::string& key) {
            auto& entry = rate_limit_state_[key];
            entry.fail_count += 1;
            entry.last_fail = now;
            entry.last_seen = now;
            if (entry.fail_count <= auth_rate_limit_burst_) {
                entry.next_allowed = now;
            } else {
                const uint32_t idx = entry.fail_count - auth_rate_limit_burst_ - 1;
                const uint32_t schedule_idx =
                    auth_rate_limit_backoff_sec_.empty()
                        ? 0
                        : std::min<size_t>(idx, auth_rate_limit_backoff_sec_.size() - 1);
                const uint32_t delay =
                    auth_rate_limit_backoff_sec_.empty()
                        ? 30
                        : auth_rate_limit_backoff_sec_[schedule_idx];
                entry.next_allowed = now + std::chrono::seconds(delay);
            }
            return entry;
        };
        const auto& per_fp = update_entry(RateLimitKey(remote, fingerprint));
        AppendRateLimitRow(remote, fingerprint, per_fp);
        const auto& per_remote = update_entry(RateLimitKey(remote, ""));
        AppendRateLimitRow(remote, "", per_remote);
        PruneRateLimitState();
    }

    void AppendRateLimitRow(const std::string& remote,
                            const std::string& fingerprint,
                            const RateLimitState& state) {
        auto db_it = databases_.find(kAuthDbName);
        if (db_it == databases_.end()) {
            return;
        }
        auto ds_it = db_it->second.datasets.find("rate_limits");
        if (ds_it == db_it->second.datasets.end()) {
            return;
        }
        const int64_t now_epoch = static_cast<int64_t>(std::time(nullptr));
        const int64_t next_epoch = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                state.next_allowed.time_since_epoch()).count());
        ds_it->second.dataset->Append({
            FieldValue::String(remote),
            FieldValue::String(fingerprint),
            FieldValue::Int32(static_cast<int32_t>(state.fail_count)),
            FieldValue::Int64(next_epoch),
            FieldValue::Int64(now_epoch),
        });
        ScheduleAuthDatasetPrune("rate_limits", auth_rate_limit_max_rows_,
                                 &rate_limit_prune_pending_);
        if (auth_prune_batch_rows_ == 0 && auth_rate_limit_max_rows_ > 0 &&
            ds_it->second.dataset->RowCount() > auth_rate_limit_max_rows_) {
            if (CompactAuthDatasetRows("rate_limits", auth_rate_limit_max_rows_)) {
                rate_limit_prune_pending_ = false;
                LoadRateLimitState();
            }
        }
    }

    void LoadRateLimitState() {
        rate_limit_state_.clear();
        const auto db_it = databases_.find(kAuthDbName);
        if (db_it == databases_.end()) {
            return;
        }
        const auto ds_it = db_it->second.datasets.find("rate_limits");
        if (ds_it == db_it->second.datasets.end()) {
            return;
        }
        const auto& schema_fields = ds_it->second.dataset->Fields();
        const auto resolve_index = [](const std::vector<FieldVector>& fields,
                                      const std::string& name,
                                      size_t fallback) -> size_t {
            for (size_t i = 0; i < fields.size(); ++i) {
                if (fields[i].Name() == name) {
                    return i;
                }
            }
            return fallback < fields.size() ? fallback : fields.size();
        };
        const size_t remote_idx = resolve_index(schema_fields, "remote_addr", 0);
        const size_t fp_idx = resolve_index(schema_fields, "fingerprint", 1);
        const size_t fail_idx = resolve_index(schema_fields, "fail_count", 2);
        const size_t next_idx = resolve_index(schema_fields, "next_allowed_at", 3);
        const size_t last_idx = resolve_index(schema_fields, "last_fail_at", 4);
        if (remote_idx >= schema_fields.size() || fp_idx >= schema_fields.size() ||
            fail_idx >= schema_fields.size() || next_idx >= schema_fields.size() ||
            last_idx >= schema_fields.size()) {
            return;
        }
        const auto scan_dataset = [&](const DatasetState& state,
                                      const std::function<void(const std::vector<FieldVector>&,
                                                               size_t)>& fn) {
            for (const auto& segment : state.dataset->Segments()) {
                const auto& fields = segment.Fields();
                for (size_t row = 0; row < segment.RowCount(); ++row) {
                    fn(fields, row);
                }
            }
            const auto& active = state.dataset->ActiveFields();
            if (!active.empty()) {
                const size_t rows = state.dataset->ActiveRowCount();
                for (size_t row = 0; row < rows; ++row) {
                    fn(active, row);
                }
            }
        };
        scan_dataset(ds_it->second, [&](const std::vector<FieldVector>& fields, size_t row) {
            std::string remote;
            std::string fp;
            int32_t fail_count = 0;
            int64_t next_allowed = 0;
            int64_t last_fail = 0;
            if (!ReadStringField(fields[remote_idx], row, &remote) ||
                !ReadStringField(fields[fp_idx], row, &fp) ||
                !ReadInt32Field(fields[fail_idx], row, &fail_count) ||
                !ReadInt64Field(fields[next_idx], row, &next_allowed) ||
                !ReadInt64Field(fields[last_idx], row, &last_fail)) {
                return;
            }
            const auto key = RateLimitKey(remote, fp);
            auto& entry = rate_limit_state_[key];
            const int64_t current_last = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    entry.last_fail.time_since_epoch()).count());
            if (entry.fail_count != 0 && last_fail <= current_last) {
                return;
            }
            entry.fail_count = static_cast<uint32_t>(fail_count);
            entry.last_fail = std::chrono::system_clock::time_point(
                std::chrono::seconds(last_fail));
            entry.next_allowed = std::chrono::system_clock::time_point(
                std::chrono::seconds(next_allowed));
            entry.last_seen = entry.last_fail;
        });
    }

    void PruneRateLimitState() {
        if (rate_limit_state_.empty()) {
            return;
        }
        const auto now = std::chrono::system_clock::now();
        if (auth_rate_limit_state_ttl_sec_ > 0) {
            const auto ttl = std::chrono::seconds(auth_rate_limit_state_ttl_sec_);
            for (auto it = rate_limit_state_.begin(); it != rate_limit_state_.end();) {
                if (it->second.last_seen.time_since_epoch().count() == 0) {
                    it->second.last_seen = now;
                }
                if (now - it->second.last_seen > ttl) {
                    it = rate_limit_state_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (auth_rate_limit_state_max_entries_ > 0 &&
            rate_limit_state_.size() > auth_rate_limit_state_max_entries_) {
            while (rate_limit_state_.size() > auth_rate_limit_state_max_entries_) {
                auto oldest = rate_limit_state_.end();
                for (auto it = rate_limit_state_.begin(); it != rate_limit_state_.end(); ++it) {
                    if (oldest == rate_limit_state_.end() ||
                        it->second.last_seen < oldest->second.last_seen) {
                        oldest = it;
                    }
                }
                if (oldest == rate_limit_state_.end()) {
                    break;
                }
                rate_limit_state_.erase(oldest);
            }
        }
    }

    void ScheduleAuthDatasetPrune(const std::string& dataset_name, size_t max_rows,
                                  bool* pending) {
        if (max_rows == 0 || pending == nullptr) {
            return;
        }
        auto db_it = databases_.find(kAuthDbName);
        if (db_it == databases_.end()) {
            return;
        }
        auto ds_it = db_it->second.datasets.find(dataset_name);
        if (ds_it == db_it->second.datasets.end()) {
            return;
        }
        const size_t row_count = ds_it->second.dataset->RowCount();
        const size_t threshold = max_rows + auth_prune_batch_rows_;
        if (row_count > threshold) {
            *pending = true;
        }
    }

    void RunAuthRetentionTasks() {
        auto needs_prune = [&](const std::string& dataset_name, size_t max_rows) -> bool {
            if (max_rows == 0) {
                return false;
            }
            auto db_it = databases_.find(kAuthDbName);
            if (db_it == databases_.end()) {
                return false;
            }
            auto ds_it = db_it->second.datasets.find(dataset_name);
            if (ds_it == db_it->second.datasets.end()) {
                return false;
            }
            return ds_it->second.dataset->RowCount() > max_rows;
        };
        if ((rate_limit_prune_pending_ ||
             needs_prune("rate_limits", auth_rate_limit_max_rows_)) &&
            CompactAuthDatasetRows("rate_limits", auth_rate_limit_max_rows_)) {
            rate_limit_prune_pending_ = false;
            LoadRateLimitState();
        }
        if ((audit_log_prune_pending_ ||
             needs_prune("audit_log", auth_audit_log_max_rows_)) &&
            CompactAuthDatasetRows("audit_log", auth_audit_log_max_rows_)) {
            audit_log_prune_pending_ = false;
        }
    }

    void AppendAuditLog(const std::string& event_type,
                        const std::string& remote,
                        const std::string& fingerprint,
                        const std::string& details_json) {
        auto db_it = databases_.find(kAuthDbName);
        if (db_it == databases_.end()) {
            return;
        }
        auto ds_it = db_it->second.datasets.find("audit_log");
        if (ds_it == db_it->second.datasets.end()) {
            return;
        }
        const int64_t now_epoch = static_cast<int64_t>(std::time(nullptr));
        ds_it->second.dataset->Append({
            FieldValue::Int64(now_epoch),
            FieldValue::String(event_type),
            FieldValue::String(remote),
            FieldValue::String(fingerprint),
            FieldValue::String(details_json),
        });
        ScheduleAuthDatasetPrune("audit_log", auth_audit_log_max_rows_,
                                 &audit_log_prune_pending_);
        if (auth_prune_batch_rows_ == 0 && auth_audit_log_max_rows_ > 0 &&
            ds_it->second.dataset->RowCount() > auth_audit_log_max_rows_) {
            if (CompactAuthDatasetRows("audit_log", auth_audit_log_max_rows_)) {
                audit_log_prune_pending_ = false;
            }
        }
    }

    bool ShouldSampleAuthz() {
        const uint64_t count = authz_denied_counter_.fetch_add(1) + 1;
        return (count % 10) == 0;
    }

    bool WriteSecureFrame(int client,
                          const std::array<uint8_t, 32>& key,
                          uint64_t seq,
                          const uint8_t* plaintext,
                          size_t plaintext_len) {
        std::vector<uint8_t> ciphertext;
        if (!AeadEncrypt(key, seq, plaintext, plaintext_len, &ciphertext)) {
            return false;
        }
        uint8_t header[12];
        std::memcpy(header, &seq, sizeof(seq));
        const uint32_t len = static_cast<uint32_t>(ciphertext.size());
        std::memcpy(header + sizeof(seq), &len, sizeof(len));
        if (!WriteExact(client, header, sizeof(header))) {
            return false;
        }
        if (!WriteExact(client, ciphertext.data(), ciphertext.size())) {
            return false;
        }
        return true;
    }

    void ApplyAuthFailureDelay(const std::chrono::steady_clock::time_point& start) const {
        if (auth_failure_delay_ms_ <= 0) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() >= auth_failure_delay_ms_) {
            return;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(auth_failure_delay_ms_ - elapsed.count()));
    }

    void PruneHandshakeNonces() {
        if (handshake_nonce_cache_.empty()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (handshake_nonce_ttl_sec_ > 0) {
            const auto ttl = std::chrono::seconds(handshake_nonce_ttl_sec_);
            for (auto it = handshake_nonce_cache_.begin();
                 it != handshake_nonce_cache_.end();) {
                if (now - it->second > ttl) {
                    it = handshake_nonce_cache_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (handshake_nonce_max_entries_ > 0 &&
            handshake_nonce_cache_.size() > handshake_nonce_max_entries_) {
            while (handshake_nonce_cache_.size() > handshake_nonce_max_entries_) {
                auto oldest = handshake_nonce_cache_.end();
                for (auto it = handshake_nonce_cache_.begin();
                     it != handshake_nonce_cache_.end(); ++it) {
                    if (oldest == handshake_nonce_cache_.end() || it->second < oldest->second) {
                        oldest = it;
                    }
                }
                if (oldest == handshake_nonce_cache_.end()) {
                    break;
                }
                handshake_nonce_cache_.erase(oldest);
            }
        }
    }

    bool RegisterHandshakeNonce(const std::string& remote, const uint8_t* nonce) {
        const std::string key = remote + "|" + HexEncode(nonce, kSecNonceBytes);
        const auto now = std::chrono::steady_clock::now();
        PruneHandshakeNonces();
        const auto it = handshake_nonce_cache_.find(key);
        if (it != handshake_nonce_cache_.end()) {
            return false;
        }
        handshake_nonce_cache_[key] = now;
        return true;
    }

    bool PerformHandshake(int client, SecureChannel* channel) {
        const auto start = std::chrono::steady_clock::now();
        constexpr size_t kClientHelloSize = 4 + 2 + 2 + kSecNonceBytes +
                                            kX25519Bytes + kEd25519PubBytes;
        std::array<uint8_t, kClientHelloSize> client_hello{};
        if (!ReadExact(client, client_hello.data(), client_hello.size())) {
            return false;
        }
        uint32_t magic = 0;
        uint16_t version = 0;
        uint16_t cipher = 0;
        std::memcpy(&magic, client_hello.data(), sizeof(magic));
        std::memcpy(&version, client_hello.data() + sizeof(magic), sizeof(version));
        std::memcpy(&cipher, client_hello.data() + sizeof(magic) + sizeof(version),
                    sizeof(cipher));
        if (magic != kSecMagic || version != kSecVersion ||
            cipher != kCipherChaCha20Poly1305) {
            ApplyAuthFailureDelay(start);
            return false;
        }
        const uint8_t* client_nonce =
            client_hello.data() + sizeof(magic) + sizeof(version) + sizeof(cipher);
        const uint8_t* client_eph_pub = client_nonce + kSecNonceBytes;
        const uint8_t* client_id_pub = client_eph_pub + kX25519Bytes;
        const std::string remote_addr = GetRemoteAddr(client);
        if (!RegisterHandshakeNonce(remote_addr, client_nonce)) {
            AppendAuditLog("auth.failed", remote_addr, "",
                           "{\"reason\":\"replay_nonce\"}");
            ApplyAuthFailureDelay(start);
            return false;
        }
        const std::string fingerprint = Sha256Hex(client_id_pub, kEd25519PubBytes);
        const int64_t wait_seconds = RateLimitWaitSeconds(remote_addr, fingerprint);

        std::array<uint8_t, kSecNonceBytes> server_nonce{};
        if (!ReadRandomBytes(server_nonce.data(), server_nonce.size())) {
            ApplyAuthFailureDelay(start);
            return false;
        }
        EVP_PKEY* server_eph = nullptr;
        std::array<uint8_t, kX25519Bytes> server_eph_pub{};
        if (!GenerateX25519Key(&server_eph, &server_eph_pub)) {
            ApplyAuthFailureDelay(start);
            return false;
        }

        uint8_t host_fingerprint_bytes[32];
        Sha256Digest(reinterpret_cast<const uint8_t*>(host_key_pub_.data()),
                     host_key_pub_.size(), host_fingerprint_bytes);

        std::vector<uint8_t> server_hello;
        server_hello.resize(4 + 2 + 2 + kSecNonceBytes + kX25519Bytes +
                            kHostKeyBytes + sizeof(host_fingerprint_bytes));
        size_t offset = 0;
        std::memcpy(server_hello.data() + offset, &magic, sizeof(magic));
        offset += sizeof(magic);
        std::memcpy(server_hello.data() + offset, &version, sizeof(version));
        offset += sizeof(version);
        std::memcpy(server_hello.data() + offset, &cipher, sizeof(cipher));
        offset += sizeof(cipher);
        std::memcpy(server_hello.data() + offset, server_nonce.data(), server_nonce.size());
        offset += server_nonce.size();
        std::memcpy(server_hello.data() + offset, server_eph_pub.data(), server_eph_pub.size());
        offset += server_eph_pub.size();
        std::memcpy(server_hello.data() + offset, host_key_pub_.data(), host_key_pub_.size());
        offset += host_key_pub_.size();
        std::memcpy(server_hello.data() + offset, host_fingerprint_bytes,
                    sizeof(host_fingerprint_bytes));
        if (!WriteExact(client, server_hello.data(), server_hello.size())) {
            EVP_PKEY_free(server_eph);
            ApplyAuthFailureDelay(start);
            return false;
        }

        std::array<uint8_t, kX25519Bytes> shared{};
        if (!DeriveX25519Secret(server_eph, client_eph_pub, kX25519Bytes, &shared)) {
            EVP_PKEY_free(server_eph);
            ApplyAuthFailureDelay(start);
            return false;
        }
        EVP_PKEY_free(server_eph);

        uint8_t salt[kSecNonceBytes * 2];
        std::memcpy(salt, client_nonce, kSecNonceBytes);
        std::memcpy(salt + kSecNonceBytes, server_nonce.data(), kSecNonceBytes);
        uint8_t okm[32 + 32 + kSecSessionIdBytes];
        const char info[] = "mimicdb-session";
        HkdfSha256(shared.data(), shared.size(), salt, sizeof(salt),
                   reinterpret_cast<const uint8_t*>(info), sizeof(info) - 1,
                   okm, sizeof(okm));
        std::memcpy(channel->key_c2s.data(), okm, 32);
        std::memcpy(channel->key_s2c.data(), okm + 32, 32);
        std::memcpy(channel->session_id.data(), okm + 64, kSecSessionIdBytes);

        std::vector<uint8_t> transcript;
        transcript.insert(transcript.end(), client_hello.begin(), client_hello.end());
        transcript.insert(transcript.end(), server_hello.begin(), server_hello.end());
        uint8_t transcript_hash[32];
        Sha256Digest(transcript.data(), transcript.size(), transcript_hash);

        std::vector<uint8_t> auth_plain;
        if (!ReadSecureFrame(client, channel->key_c2s, 0, &auth_plain)) {
            RecordRateLimitFailure(remote_addr, fingerprint);
            AppendAuditLog("auth.failed", remote_addr, fingerprint,
                           "{\"reason\":\"read_auth\"}");
            ApplyAuthFailureDelay(start);
            return false;
        }
        if (auth_plain.size() != kEd25519SigBytes) {
            RecordRateLimitFailure(remote_addr, fingerprint);
            AppendAuditLog("auth.failed", remote_addr, fingerprint,
                           "{\"reason\":\"bad_signature_len\"}");
            ApplyAuthFailureDelay(start);
            return false;
        }
        if (!VerifyEd25519(client_id_pub, kEd25519PubBytes,
                           transcript_hash, sizeof(transcript_hash),
                           auth_plain.data(), auth_plain.size())) {
            RecordRateLimitFailure(remote_addr, fingerprint);
            AppendAuditLog("auth.failed", remote_addr, fingerprint,
                           "{\"reason\":\"bad_signature\"}");
            ApplyAuthFailureDelay(start);
            return false;
        }

        AuthSnapshot snapshot;
        if (!LoadAuthSnapshot(&snapshot)) {
            RecordRateLimitFailure(remote_addr, fingerprint);
            AppendAuditLog("auth.failed", remote_addr, fingerprint,
                           "{\"reason\":\"auth_db\"}");
            ApplyAuthFailureDelay(start);
            return false;
        }
        auto send_rate_limited = [&]() {
            std::vector<uint8_t> reject;
            reject.reserve(1 + sizeof(uint32_t) + sizeof(uint32_t) * 2 +
                           channel->session_id.size());
            const uint8_t status = 2;
            const uint32_t wait = static_cast<uint32_t>(wait_seconds);
            const uint32_t idle = 0;
            const uint32_t max_lifetime = 0;
            reject.push_back(status);
            reject.insert(reject.end(),
                          reinterpret_cast<const uint8_t*>(&wait),
                          reinterpret_cast<const uint8_t*>(&wait) + sizeof(wait));
            reject.insert(reject.end(),
                          reinterpret_cast<const uint8_t*>(&idle),
                          reinterpret_cast<const uint8_t*>(&idle) + sizeof(idle));
            reject.insert(reject.end(),
                          reinterpret_cast<const uint8_t*>(&max_lifetime),
                          reinterpret_cast<const uint8_t*>(&max_lifetime) +
                              sizeof(max_lifetime));
            reject.insert(reject.end(), channel->session_id.begin(), channel->session_id.end());
            WriteSecureFrame(client, channel->key_s2c, 0, reject.data(), reject.size());
            AppendAuditLog("auth.failed", remote_addr, fingerprint,
                           "{\"reason\":\"rate_limited\"}");
        };
        auto key_it = snapshot.key_enabled.find(fingerprint);
        const bool key_enabled = key_it != snapshot.key_enabled.end() && key_it->second;
        if (!key_enabled) {
            if (!IsRootInitialized(auth_db_path_) && IsLocalRemote(remote_addr)) {
                SessionInfo info;
                info.session_id_hex =
                    BytesToHex(channel->session_id.data(), channel->session_id.size());
                info.fingerprint = fingerprint;
                info.permissions = PermissionSet{};
                info.created_at = std::chrono::steady_clock::now();
                info.last_activity = info.created_at;
                AddSession(info);
            } else {
                if (wait_seconds > 0) {
                    send_rate_limited();
                    return false;
                }
                RecordRateLimitFailure(remote_addr, fingerprint);
                AppendAuditLog("auth.failed", remote_addr, fingerprint,
                               "{\"reason\":\"key_disabled\"}");
                ApplyAuthFailureDelay(start);
                return false;
            }
        }

        std::vector<uint8_t> server_transcript;
        server_transcript.insert(server_transcript.end(), transcript.begin(), transcript.end());
        server_transcript.insert(server_transcript.end(), auth_plain.begin(), auth_plain.end());
        uint8_t server_hash[32];
        Sha256Digest(server_transcript.data(), server_transcript.size(), server_hash);
        std::array<uint8_t, kEd25519SigBytes> server_sig{};
        if (!SignEd25519(host_key_priv_.data(), host_key_priv_.size(),
                         server_hash, sizeof(server_hash), &server_sig)) {
            RecordRateLimitFailure(remote_addr, fingerprint);
            AppendAuditLog("auth.failed", remote_addr, fingerprint,
                           "{\"reason\":\"server_sig\"}");
            ApplyAuthFailureDelay(start);
            return false;
        }

        const uint16_t fp_len = static_cast<uint16_t>(fingerprint.size());
        std::vector<uint8_t> accept;
        accept.reserve(1 + sizeof(uint32_t) * 3 + channel->session_id.size() +
                       sizeof(uint16_t) + fp_len + server_sig.size());
        const uint8_t status = 1;
        const uint32_t wait = 0;
        const uint32_t idle = session_idle_timeout_sec_;
        const uint32_t max_lifetime = session_max_lifetime_sec_;
        accept.push_back(status);
        accept.insert(accept.end(),
                      reinterpret_cast<const uint8_t*>(&wait),
                      reinterpret_cast<const uint8_t*>(&wait) + sizeof(wait));
        accept.insert(accept.end(),
                      reinterpret_cast<const uint8_t*>(&idle),
                      reinterpret_cast<const uint8_t*>(&idle) + sizeof(idle));
        accept.insert(accept.end(),
                      reinterpret_cast<const uint8_t*>(&max_lifetime),
                      reinterpret_cast<const uint8_t*>(&max_lifetime) + sizeof(max_lifetime));
        accept.insert(accept.end(), channel->session_id.begin(), channel->session_id.end());
        accept.insert(accept.end(),
                      reinterpret_cast<const uint8_t*>(&fp_len),
                      reinterpret_cast<const uint8_t*>(&fp_len) + sizeof(fp_len));
        accept.insert(accept.end(),
                      reinterpret_cast<const uint8_t*>(fingerprint.data()),
                      reinterpret_cast<const uint8_t*>(fingerprint.data()) + fp_len);
        accept.insert(accept.end(), server_sig.begin(), server_sig.end());
        if (!WriteSecureFrame(client, channel->key_s2c, 0, accept.data(), accept.size())) {
            ApplyAuthFailureDelay(start);
            return false;
        }

        channel->recv_seq = 1;
        channel->send_seq = 1;
        std::vector<uint8_t> ack_plain;
        if (!ReadSecureFrame(client, channel->key_c2s, channel->recv_seq, &ack_plain)) {
            RecordRateLimitFailure(remote_addr, fingerprint);
            AppendAuditLog("auth.failed", remote_addr, fingerprint,
                           "{\"reason\":\"ack_read\"}");
            ApplyAuthFailureDelay(start);
            return false;
        }
        channel->recv_seq += 1;
        size_t ack_cursor = 0;
        std::string ack_fingerprint;
        if (!ReadName(ack_plain, &ack_cursor, &ack_fingerprint) ||
            ack_cursor != ack_plain.size() || ack_fingerprint != fingerprint) {
            RecordRateLimitFailure(remote_addr, fingerprint);
            AppendAuditLog("auth.failed", remote_addr, fingerprint,
                           "{\"reason\":\"ack_mismatch\"}");
            ApplyAuthFailureDelay(start);
            return false;
        }

        const std::string session_id_hex =
            BytesToHex(channel->session_id.data(), channel->session_id.size());
        bool has_session = false;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            has_session = session_store_.count(session_id_hex) != 0;
        }
        if (!has_session) {
            SessionInfo info;
            info.session_id_hex = session_id_hex;
            info.fingerprint = fingerprint;
            info.permissions = BuildPermissions(fingerprint);
            info.created_at = std::chrono::steady_clock::now();
            info.last_activity = info.created_at;
            AddSession(info);
        }
        AppendAuditLog("auth.login", remote_addr, fingerprint,
                       "{\"session\":\"" + session_id_hex + "\"}");
        ResetRateLimit(remote_addr, fingerprint);
        return true;
    }

    bool ReadSecureMessage(int client, SecureChannel* channel,
                           MessageHeader* header,
                           std::vector<uint8_t>* payload) {
        std::vector<uint8_t> plain;
        if (!ReadSecureFrame(client, channel->key_c2s, channel->recv_seq, &plain)) {
            return false;
        }
        channel->recv_seq += 1;
        if (plain.size() < sizeof(MessageHeader)) {
            return false;
        }
        std::memcpy(header, plain.data(), sizeof(MessageHeader));
        if (header->magic != kMagic || header->version != kVersion) {
            return false;
        }
        if (max_payload_bytes_ > 0 && header->payload_size > max_payload_bytes_) {
            return false;
        }
        const size_t expected_size = sizeof(MessageHeader) + header->payload_size;
        if (plain.size() != expected_size) {
            return false;
        }
        payload->assign(plain.begin() + sizeof(MessageHeader), plain.end());
        if (header->flags & kFlagSessionId) {
            if (payload->size() < kSecSessionIdBytes) {
                return false;
            }
            if (std::memcmp(payload->data(), channel->session_id.data(),
                            kSecSessionIdBytes) != 0) {
                return false;
            }
            payload->erase(payload->begin(), payload->begin() + kSecSessionIdBytes);
            header->payload_size =
                static_cast<uint32_t>(payload->size());
        }
        return true;
    }

    bool IsLocalRemote(const std::string& remote) const {
        return remote == "127.0.0.1";
    }

    bool ParseKeyRegistrationPayload(const std::vector<uint8_t>& payload,
                                     std::string* out_pubkey,
                                     std::string* out_comment,
                                     std::string* out_claimed_fingerprint) const {
        size_t cursor = 0;
        uint16_t key_len = 0;
        if (!ReadScalar(payload, &cursor, &key_len)) {
            return false;
        }
        if (cursor + key_len > payload.size()) {
            return false;
        }
        out_pubkey->assign(reinterpret_cast<const char*>(payload.data() + cursor), key_len);
        cursor += key_len;
        out_comment->clear();
        if (cursor + sizeof(uint16_t) <= payload.size()) {
            uint16_t comment_len = 0;
            if (!ReadScalar(payload, &cursor, &comment_len)) {
                return false;
            }
            if (cursor + comment_len > payload.size()) {
                return false;
            }
            out_comment->assign(reinterpret_cast<const char*>(payload.data() + cursor),
                                comment_len);
            cursor += comment_len;
        }
        out_claimed_fingerprint->clear();
        if (cursor < payload.size()) {
            const uint8_t tag = payload[cursor];
            if (tag != kKeyConfirmTag) {
                return false;
            }
            cursor += 1;
            if (!ReadName(payload, &cursor, out_claimed_fingerprint)) {
                return false;
            }
        }
        return cursor == payload.size();
    }

    void AppendKeyRecord(const std::string& fingerprint,
                         const std::string& public_key,
                         const std::string& comment,
                         bool enabled) {
        auto db_it = databases_.find(kAuthDbName);
        if (db_it == databases_.end()) {
            return;
        }
        auto ds_it = db_it->second.datasets.find("keys");
        if (ds_it == db_it->second.datasets.end()) {
            return;
        }
        const int64_t now_epoch = static_cast<int64_t>(std::time(nullptr));
        ds_it->second.dataset->Append({
            FieldValue::String(""),
            FieldValue::String(fingerprint),
            FieldValue::Bytes(public_key),
            FieldValue::String(comment),
            FieldValue::Bool(enabled),
            FieldValue::Int64(now_epoch),
            FieldValue::Int64(now_epoch),
        });
    }

    void AppendRoleRecord(const std::string& role_name, bool built_in) {
        auto db_it = databases_.find(kAuthDbName);
        if (db_it == databases_.end()) {
            return;
        }
        auto ds_it = db_it->second.datasets.find("roles");
        if (ds_it == db_it->second.datasets.end()) {
            return;
        }
        const int64_t now_epoch = static_cast<int64_t>(std::time(nullptr));
        ds_it->second.dataset->Append({
            FieldValue::String(role_name),
            FieldValue::Int64(now_epoch),
            FieldValue::Bool(built_in),
        });
    }

    void AppendRoleGrant(const std::string& role_name,
                         const std::string& capability,
                         const std::string& scope) {
        auto db_it = databases_.find(kAuthDbName);
        if (db_it == databases_.end()) {
            return;
        }
        auto ds_it = db_it->second.datasets.find("role_grants");
        if (ds_it == db_it->second.datasets.end()) {
            return;
        }
        ds_it->second.dataset->Append({
            FieldValue::String(role_name),
            FieldValue::String(capability),
            FieldValue::String(scope),
        });
    }

    void AppendKeyRole(const std::string& fingerprint,
                       const std::string& role_name,
                       const std::string& scope) {
        auto db_it = databases_.find(kAuthDbName);
        if (db_it == databases_.end()) {
            return;
        }
        auto ds_it = db_it->second.datasets.find("key_roles");
        if (ds_it == db_it->second.datasets.end()) {
            return;
        }
        ds_it->second.dataset->Append({
            FieldValue::String(fingerprint),
            FieldValue::String(role_name),
            FieldValue::String(scope),
        });
    }

    void AppendKeyGrant(const std::string& fingerprint,
                        const std::string& capability,
                        const std::string& scope) {
        auto db_it = databases_.find(kAuthDbName);
        if (db_it == databases_.end()) {
            return;
        }
        auto ds_it = db_it->second.datasets.find("key_grants");
        if (ds_it == db_it->second.datasets.end()) {
            return;
        }
        ds_it->second.dataset->Append({
            FieldValue::String(fingerprint),
            FieldValue::String(capability),
            FieldValue::String(scope),
        });
    }
    void HandleClient(int client) {
        SecureChannel channel;
        if (!PerformHandshake(client, &channel)) {
            return;
        }
        const std::string session_id_hex =
            BytesToHex(channel.session_id.data(), channel.session_id.size());
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            connection_states_[client] = ConnectionState{channel, session_id_hex};
        }
        for (;;) {
            MessageHeader header{};
            std::vector<uint8_t> payload;
            if (!ReadSecureMessage(client, &channel, &header, &payload)) {
                break;
            }
            Dispatch(client, header, payload);
        }
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            connection_states_.erase(client);
        }
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            session_store_.erase(session_id_hex);
        }
    }

    void Dispatch(int client, const MessageHeader& header,
                  const std::vector<uint8_t>& payload) {
        const OpCode opcode = static_cast<OpCode>(header.opcode);
        if (!AuthorizeRequest(client, header, opcode, payload)) {
            return;
        }
        switch (opcode) {
            case OpCode::kPing:
                SendStatus(client, header, Status::kOk, {});
                return;
            case OpCode::kCreateDatabase:
                HandleCreateDatabase(client, header, payload);
                return;
            case OpCode::kListDatabases:
                HandleListDatabases(client, header);
                return;
            case OpCode::kDropDatabase:
                HandleDropDatabase(client, header, payload);
                return;
            case OpCode::kCreateDataset:
                HandleCreateDataset(client, header, payload);
                return;
            case OpCode::kDropDataset:
                HandleDropDataset(client, header, payload);
                return;
            case OpCode::kAppendBatch:
                HandleAppendBatch(client, header, payload);
                return;
            case OpCode::kQueryAgg:
                HandleQueryAgg(client, header, payload);
                return;
            case OpCode::kVectorSearch:
                HandleVectorSearch(client, header, payload);
                return;
            case OpCode::kScan:
                HandleScan(client, header, payload);
                return;
            case OpCode::kHealth:
                HandleHealth(client, header);
                return;
            case OpCode::kHostKey:
                HandleHostKey(client, header);
                return;
            case OpCode::kHostKeyRotate:
                HandleHostKeyRotate(client, header);
                return;
            case OpCode::kAuthInitRoot:
                HandleAuthInitRoot(client, header, payload);
                return;
            case OpCode::kAuthKeyAdd:
                HandleAuthKeyAdd(client, header, payload);
                return;
            case OpCode::kAuthKeyDisable:
                HandleAuthKeyDisable(client, header, payload, false);
                return;
            case OpCode::kAuthKeyRemove:
                HandleAuthKeyDisable(client, header, payload, true);
                return;
            case OpCode::kAuthKeyList:
                HandleAuthKeyList(client, header);
                return;
            case OpCode::kAuthRoleCreate:
                HandleAuthRoleCreate(client, header, payload);
                return;
            case OpCode::kAuthRoleDelete:
                HandleAuthRoleDelete(client, header, payload);
                return;
            case OpCode::kAuthRoleGrant:
                HandleAuthRoleGrant(client, header, payload, false);
                return;
            case OpCode::kAuthRoleRevoke:
                HandleAuthRoleGrant(client, header, payload, true);
                return;
            case OpCode::kAuthAssignRole:
                HandleAuthAssignRole(client, header, payload, false);
                return;
            case OpCode::kAuthUnassignRole:
                HandleAuthAssignRole(client, header, payload, true);
                return;
            case OpCode::kAuthGrantKey:
                HandleAuthGrantKey(client, header, payload, false);
                return;
            case OpCode::kAuthRevokeKeyGrant:
                HandleAuthGrantKey(client, header, payload, true);
                return;
            case OpCode::kAuthRateLimitList:
                HandleAuthRateLimitList(client, header);
                return;
            case OpCode::kAuthRateLimitClear:
                HandleAuthRateLimitClear(client, header, payload);
                return;
            case OpCode::kAuthWhoami:
                HandleAuthWhoami(client, header);
                return;
            default:
                SendStatus(client, header, Status::kUnsupported, {});
                return;
        }
    }

    bool GetSessionForClient(int client, SessionInfo* out) {
        std::string session_id;
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            auto it = connection_states_.find(client);
            if (it == connection_states_.end()) {
                return false;
            }
            session_id = it->second.session_id;
        }
        std::lock_guard<std::mutex> lock(session_mutex_);
        auto it = session_store_.find(session_id);
        if (it == session_store_.end()) {
            return false;
        }
        *out = it->second;
        return true;
    }

    bool UpdateSessionActivity(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        auto it = session_store_.find(session_id);
        if (it == session_store_.end()) {
            return false;
        }
        it->second.last_activity = std::chrono::steady_clock::now();
        return true;
    }

    void RemoveSession(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_store_.erase(session_id);
    }

    void AddSession(const SessionInfo& info) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_store_[info.session_id_hex] = info;
        PruneSessionsLocked();
    }

    void PruneSessions() {
        std::lock_guard<std::mutex> lock(session_mutex_);
        PruneSessionsLocked();
    }

    void PruneSessionsLocked() {
        const auto now = std::chrono::steady_clock::now();
        if (session_idle_timeout_sec_ > 0 || session_max_lifetime_sec_ > 0) {
            for (auto it = session_store_.begin(); it != session_store_.end();) {
                bool expired = false;
                if (session_idle_timeout_sec_ > 0) {
                    const auto idle =
                        std::chrono::duration_cast<std::chrono::seconds>(
                            now - it->second.last_activity).count();
                    if (idle > session_idle_timeout_sec_) {
                        expired = true;
                    }
                }
                if (!expired && session_max_lifetime_sec_ > 0) {
                    const auto age =
                        std::chrono::duration_cast<std::chrono::seconds>(
                            now - it->second.created_at).count();
                    if (age > session_max_lifetime_sec_) {
                        expired = true;
                    }
                }
                if (expired) {
                    it = session_store_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (session_max_per_fingerprint_ > 0 && !session_store_.empty()) {
            std::unordered_map<std::string,
                std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>>> buckets;
            for (const auto& entry : session_store_) {
                buckets[entry.second.fingerprint].push_back(
                    {entry.second.last_activity, entry.first});
            }
            for (auto& bucket : buckets) {
                auto& sessions = bucket.second;
                if (sessions.size() <= session_max_per_fingerprint_) {
                    continue;
                }
                std::sort(sessions.begin(), sessions.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
                size_t to_remove = sessions.size() - session_max_per_fingerprint_;
                for (size_t i = 0; i < to_remove; ++i) {
                    session_store_.erase(sessions[i].second);
                }
            }
        }

        if (session_max_total_ > 0 && session_store_.size() > session_max_total_) {
            std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>> ordered;
            ordered.reserve(session_store_.size());
            for (const auto& entry : session_store_) {
                ordered.push_back({entry.second.last_activity, entry.first});
            }
            std::sort(ordered.begin(), ordered.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            const size_t to_remove = ordered.size() - session_max_total_;
            for (size_t i = 0; i < to_remove; ++i) {
                session_store_.erase(ordered[i].second);
            }
        }
    }

    bool AuthorizeRequest(int client, const MessageHeader& header, OpCode opcode,
                          const std::vector<uint8_t>& payload) {
        SessionInfo session;
        if (!GetSessionForClient(client, &session)) {
            if (opcode == OpCode::kAuthInitRoot &&
                !IsRootInitialized(auth_db_path_) &&
                IsLocalRemote(GetRemoteAddr(client))) {
                return true;
            }
            SendStatus(client, header, Status::kAuthFailed, {});
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (session_idle_timeout_sec_ > 0) {
            const auto idle =
                std::chrono::duration_cast<std::chrono::seconds>(now - session.last_activity)
                    .count();
            if (idle >= session_idle_timeout_sec_) {
                RemoveSession(session.session_id_hex);
                SendStatus(client, header, Status::kAuthFailed, {});
                return false;
            }
        }
        if (session_max_lifetime_sec_ > 0) {
            const auto age =
                std::chrono::duration_cast<std::chrono::seconds>(now - session.created_at)
                    .count();
            if (age >= session_max_lifetime_sec_) {
                RemoveSession(session.session_id_hex);
                SendStatus(client, header, Status::kAuthFailed, {});
                return false;
            }
        }
        std::vector<std::pair<std::string, std::string>> required;
        std::string db_name;
        std::string dataset_name;
        size_t cursor = 0;
        const auto db_scope = [&](const std::string& db) {
            return "database:" + db;
        };
        const auto dataset_scope = [&](const std::string& db, const std::string& ds) {
            return "dataset:" + db + "." + ds;
        };
        switch (opcode) {
            case OpCode::kPing:
            case OpCode::kHealth:
                required.push_back({"server.health", "*"});
                break;
            case OpCode::kHostKey:
                break;
            case OpCode::kHostKeyRotate:
                required.push_back({"server.key.rotate", "*"});
                break;
            case OpCode::kAuthInitRoot:
                if (!IsRootInitialized(auth_db_path_) &&
                    IsLocalRemote(GetRemoteAddr(client))) {
                    break;
                }
                required.push_back({"auth.manage", "*"});
                break;
            case OpCode::kAuthKeyAdd:
            case OpCode::kAuthKeyDisable:
            case OpCode::kAuthKeyRemove:
            case OpCode::kAuthRoleCreate:
            case OpCode::kAuthRoleDelete:
            case OpCode::kAuthRoleGrant:
            case OpCode::kAuthRoleRevoke:
            case OpCode::kAuthAssignRole:
            case OpCode::kAuthUnassignRole:
            case OpCode::kAuthGrantKey:
            case OpCode::kAuthRevokeKeyGrant:
            case OpCode::kAuthRateLimitClear:
                required.push_back({"auth.manage", "*"});
                break;
            case OpCode::kAuthKeyList:
            case OpCode::kAuthRateLimitList:
                required.push_back({"auth.read", "*"});
                break;
            case OpCode::kAuthWhoami:
                break;
            case OpCode::kListDatabases:
                required.push_back({"db.list", "*"});
                break;
            case OpCode::kCreateDatabase:
                if (!ReadName(payload, &cursor, &db_name)) {
                    SendStatus(client, header, Status::kBadRequest, {});
                    return false;
                }
                required.push_back({"db.create", "*"});
                break;
            case OpCode::kDropDatabase:
                if (!ReadName(payload, &cursor, &db_name)) {
                    SendStatus(client, header, Status::kBadRequest, {});
                    return false;
                }
                required.push_back({"db.drop", db_scope(db_name)});
                break;
            case OpCode::kCreateDataset:
                if (!ReadName(payload, &cursor, &db_name)) {
                    SendStatus(client, header, Status::kBadRequest, {});
                    return false;
                }
                required.push_back({"dataset.create", db_scope(db_name)});
                break;
            case OpCode::kDropDataset:
                if (!ReadName(payload, &cursor, &db_name) ||
                    !ReadName(payload, &cursor, &dataset_name)) {
                    SendStatus(client, header, Status::kBadRequest, {});
                    return false;
                }
                required.push_back({"dataset.drop", dataset_scope(db_name, dataset_name)});
                break;
            case OpCode::kAppendBatch:
                if (!ReadName(payload, &cursor, &db_name) ||
                    !ReadName(payload, &cursor, &dataset_name)) {
                    SendStatus(client, header, Status::kBadRequest, {});
                    return false;
                }
                required.push_back({"dataset.write", dataset_scope(db_name, dataset_name)});
                break;
            case OpCode::kQueryAgg:
            case OpCode::kVectorSearch:
                if (!ReadName(payload, &cursor, &db_name) ||
                    !ReadName(payload, &cursor, &dataset_name)) {
                    SendStatus(client, header, Status::kBadRequest, {});
                    return false;
                }
                required.push_back({opcode == OpCode::kVectorSearch ? "query.vector" : "query.aggregate",
                                    dataset_scope(db_name, dataset_name)});
                required.push_back({"dataset.read", dataset_scope(db_name, dataset_name)});
                break;
            case OpCode::kScan:
                if (!ReadName(payload, &cursor, &db_name) ||
                    !ReadName(payload, &cursor, &dataset_name)) {
                    SendStatus(client, header, Status::kBadRequest, {});
                    return false;
                }
                if (IsAuthDatabaseName(db_name)) {
                    if (!IsAuthorized(session.permissions, "auth.read", "*") &&
                        !IsAuthorized(session.permissions, "auth.manage", "*")) {
                        SendStatus(client, header, Status::kNotFound, {});
                        return false;
                    }
                    break;
                }
                required.push_back({"query.scan", dataset_scope(db_name, dataset_name)});
                required.push_back({"dataset.read", dataset_scope(db_name, dataset_name)});
                break;
            default:
                break;
        }

        for (const auto& req : required) {
            if (!IsAuthorized(session.permissions, req.first, req.second)) {
                if (ShouldSampleAuthz()) {
                    const std::string remote = GetRemoteAddr(client);
                    const std::string details =
                        std::string("{\"opcode\":") +
                        std::to_string(static_cast<uint16_t>(opcode)) +
                        ",\"cap\":\"" + req.first + "\",\"scope\":\"" + req.second + "\"}";
                    AppendAuditLog("authz.denied", remote, session.fingerprint, details);
                }
                SendPermissionDenied(client, header, req.first, req.second);
                return false;
            }
        }
        UpdateSessionActivity(session.session_id_hex);
        return true;
    }

    void HandleCreateDataset(int client, const MessageHeader& header,
                             const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string db_name;
        if (!ReadName(payload, &cursor, &db_name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::string name;
        if (!ReadName(payload, &cursor, &name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        uint16_t field_count = 0;
        if (!ReadScalar(payload, &cursor, &field_count)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        auto db_it = databases_.find(db_name);
        if (db_it == databases_.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        if (db_it->second.datasets.find(name) != db_it->second.datasets.end()) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        auto dataset = std::make_unique<Dataset>(name);
        for (uint16_t i = 0; i < field_count; ++i) {
            uint16_t field_name_len = 0;
            if (!ReadScalar(payload, &cursor, &field_name_len)) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            std::string field_name;
            if (!ReadBytes(payload, &cursor, field_name_len, &field_name)) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            uint8_t field_type = 0;
            if (!ReadScalar(payload, &cursor, &field_type)) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            dataset->AddField(FieldVector(field_name, DecodeFieldType(field_type)));
        }
        DatasetState state;
        state.dataset = std::move(dataset);
        state.path = DatasetPath(db_name, name);
        if (!std::filesystem::exists(state.path)) {
            std::filesystem::create_directories(state.path);
        }
        if (!WriteSchema(state.path, *state.dataset)) {
            SendStatus(client, header, Status::kInternalError, {});
            return;
        }
        db_it->second.datasets[name] = std::move(state);
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleDropDatabase(int client, const MessageHeader& header,
                            const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string db_name;
        if (!ReadName(payload, &cursor, &db_name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        if (IsAuthDatabaseName(db_name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        auto it = databases_.find(db_name);
        if (it == databases_.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(DatabasePath(db_name), ec);
        databases_.erase(it);
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleDropDataset(int client, const MessageHeader& header,
                           const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string db_name;
        if (!ReadName(payload, &cursor, &db_name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        if (IsAuthDatabaseName(db_name)) {
            SessionInfo session;
            if (!GetSessionForClient(client, &session) ||
                (!IsAuthorized(session.permissions, "auth.read", "*") &&
                 !IsAuthorized(session.permissions, "auth.manage", "*"))) {
                SendStatus(client, header, Status::kNotFound, {});
                return;
            }
        }
        std::string name;
        if (!ReadName(payload, &cursor, &name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        auto db_it = databases_.find(db_name);
        if (db_it == databases_.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        auto it = db_it->second.datasets.find(name);
        if (it == db_it->second.datasets.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(DatasetPath(db_name, name), ec);
        db_it->second.datasets.erase(it);
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleAppendBatch(int client, const MessageHeader& header,
                           const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string db_name;
        if (!ReadName(payload, &cursor, &db_name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::string name;
        if (!ReadName(payload, &cursor, &name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        uint64_t batch_id = 0;
        if (!ReadScalar(payload, &cursor, &batch_id)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        auto db_it = databases_.find(db_name);
        if (db_it == databases_.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        auto it = db_it->second.datasets.find(name);
        if (it == db_it->second.datasets.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        if (batch_id != 0 && it->second.seen_batches.find(batch_id) != it->second.seen_batches.end()) {
            SendStatus(client, header, Status::kOk, {});
            return;
        }
        uint32_t row_count = 0;
        uint16_t field_count = 0;
        if (!ReadScalar(payload, &cursor, &row_count) ||
            !ReadScalar(payload, &cursor, &field_count)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        if (max_rows_per_batch_ > 0 && row_count > max_rows_per_batch_) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::vector<FieldBatch> batches(field_count);
        std::vector<std::vector<uint8_t>> batch_storage(field_count);
        std::vector<std::vector<uint8_t>> validity_storage(field_count);
        std::vector<std::vector<uint8_t>> bytes_storage(field_count);
        std::vector<std::vector<uint8_t>> length_storage(field_count);
        for (uint16_t i = 0; i < field_count; ++i) {
            uint16_t field_index = 0;
            uint8_t field_type = 0;
            uint8_t validity_mode = 0;
            uint32_t element_count = 0;
            if (!ReadScalar(payload, &cursor, &field_index) ||
                !ReadScalar(payload, &cursor, &field_type) ||
                !ReadScalar(payload, &cursor, &validity_mode) ||
                !ReadScalar(payload, &cursor, &element_count)) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            if (element_count != row_count || field_index >= field_count) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            const FieldType type = DecodeFieldType(field_type);
            size_t data_bytes = 0;
            if (type == FieldType::kString || type == FieldType::kBytes ||
                type == FieldType::kVectorFloat32) {
                uint32_t bytes_size = 0;
                if (!ReadScalar(payload, &cursor, &bytes_size)) {
                    SendStatus(client, header, Status::kBadRequest, {});
                    return;
                }
                const size_t length_bytes = static_cast<size_t>(element_count) * sizeof(uint32_t);
                if (cursor + length_bytes + bytes_size > payload.size()) {
                    SendStatus(client, header, Status::kBadRequest, {});
                    return;
                }
                length_storage[field_index].resize(length_bytes);
                std::memcpy(length_storage[field_index].data(), payload.data() + cursor,
                            length_bytes);
                cursor += length_bytes;
                bytes_storage[field_index].resize(bytes_size);
                if (bytes_size > 0) {
                    std::memcpy(bytes_storage[field_index].data(), payload.data() + cursor,
                                bytes_size);
                }
                cursor += bytes_size;
                data_bytes = bytes_size;
            } else {
                const size_t type_size = TypeSize(type);
                data_bytes = static_cast<size_t>(element_count) * type_size;
                if (cursor + data_bytes > payload.size()) {
                    SendStatus(client, header, Status::kBadRequest, {});
                    return;
                }
                batch_storage[field_index].resize(data_bytes);
                std::memcpy(batch_storage[field_index].data(), payload.data() + cursor, data_bytes);
                cursor += data_bytes;
            }

            const uint8_t* validity_ptr = nullptr;
            if (validity_mode == 1) {
                const size_t validity_bytes = (element_count + 7) / 8;
                if (cursor + validity_bytes > payload.size()) {
                    SendStatus(client, header, Status::kBadRequest, {});
                    return;
                }
                std::vector<uint8_t> packed_validity(validity_bytes);
                std::memcpy(packed_validity.data(), payload.data() + cursor, validity_bytes);
                cursor += validity_bytes;
                validity_storage[field_index].resize(element_count);
                for (size_t j = 0; j < element_count; ++j) {
                    const uint8_t byte = packed_validity[j / 8];
                    validity_storage[field_index][j] = (byte >> (j % 8)) & 1U;
                }
                validity_ptr = validity_storage[field_index].data();
            } else if (validity_mode != 0) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            batches[field_index] = FieldBatch{
                type,
                batch_storage[field_index].empty() ? nullptr : batch_storage[field_index].data(),
                element_count,
                validity_ptr,
                length_storage[field_index].empty()
                    ? nullptr
                    : reinterpret_cast<const uint32_t*>(length_storage[field_index].data()),
                bytes_storage[field_index].empty() ? nullptr : bytes_storage[field_index].data(),
                data_bytes,
            };
        }
        if (!it->second.dataset->AppendBatch(batches)) {
            SendStatus(client, header, Status::kInternalError, {});
            return;
        }
        if (!PersistNewSegments(db_name, it->second)) {
            SendStatus(client, header, Status::kInternalError, {});
            return;
        }
        if (batch_id != 0) {
            it->second.seen_batches.insert(batch_id);
        }
        if (append_sleep_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(append_sleep_ms_));
        }
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleQueryAgg(int client, const MessageHeader& header,
                        const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string db_name;
        if (!ReadName(payload, &cursor, &db_name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::string name;
        if (!ReadName(payload, &cursor, &name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        auto db_it = databases_.find(db_name);
        if (db_it == databases_.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        auto it = db_it->second.datasets.find(name);
        if (it == db_it->second.datasets.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        uint16_t field_index = 0;
        if (!ReadScalar(payload, &cursor, &field_index)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        uint16_t predicate_count = 0;
        if (!ReadPredicateCount(payload, &cursor, &predicate_count)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::vector<PredicateSpec> predicates;
        predicates.reserve(predicate_count);
        for (uint16_t i = 0; i < predicate_count; ++i) {
            uint16_t pred_field = 0;
            uint8_t op = 0;
            double value = 0.0;
            if (!ReadScalar(payload, &cursor, &pred_field) ||
                !ReadScalar(payload, &cursor, &op) ||
                !ReadScalar(payload, &cursor, &value)) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            predicates.push_back(PredicateSpec{pred_field, DecodeCompareOp(op), value});
        }
        DatasetState& state = it->second;
        const Dataset& dataset = *state.dataset;
        if (field_index >= dataset.Fields().size()) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        const auto agg_type = dataset.Fields()[field_index].Type();
        if (agg_type == FieldType::kString || agg_type == FieldType::kBytes) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        AggregateResult result{};
        const size_t persisted = state.persisted_segments;
        size_t thread_count = query_threads_ == 0 ? 1 : query_threads_;
        if (thread_count > persisted) {
            thread_count = persisted == 0 ? 1 : persisted;
        }
        if (thread_count > 1 && persisted > 0) {
            std::atomic<bool> ok{true};
            std::vector<AggregateResult> partials(thread_count);
            std::vector<std::thread> threads;
            threads.reserve(thread_count);
            for (size_t t = 0; t < thread_count; ++t) {
                const size_t start = (persisted * t) / thread_count;
                const size_t end = (persisted * (t + 1)) / thread_count;
                threads.emplace_back([&, start, end, t]() {
                    AggregateResult local{};
                    for (size_t idx = start; idx < end && ok.load(); ++idx) {
                        const size_t base = state.segment_base_index;
                        const size_t in_count = state.dataset->Segments().size();
                        const size_t in_end = base + in_count;
                        const Segment* segment_ptr = nullptr;
                        Segment segment(0, {});
                        if (idx >= base && idx < in_end) {
                            segment_ptr = &state.dataset->Segments()[idx - base];
                        } else {
                            SegmentReader reader(SegmentPath(db_name, state.dataset->Name(), idx));
                            if (!reader.Read(&segment)) {
                                ok.store(false);
                                return;
                            }
                            segment_ptr = &segment;
                        }
                        if (field_index >= segment_ptr->Fields().size()) {
                            ok.store(false);
                            return;
                        }
                        AggregateResult partial{};
                        if (!predicates.empty()) {
                            Mask mask;
                            if (!BuildPredicateMask(segment_ptr->Fields(), predicates, &mask)) {
                                ok.store(false);
                                return;
                            }
                            AggregateMixed(segment_ptr->Fields()[field_index], &mask, &partial);
                        } else {
                            AggregateMixed(segment_ptr->Fields()[field_index], nullptr, &partial);
                        }
                        MergeAggregate(partial, &local);
                    }
                    partials[t] = local;
                });
            }
            for (auto& thread : threads) {
                thread.join();
            }
            if (!ok.load()) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            for (const auto& partial : partials) {
                MergeAggregate(partial, &result);
            }
        } else {
            if (!ForEachSegment(
                    db_name, state,
                    [&](const Segment& segment) -> bool {
                        if (field_index >= segment.Fields().size()) {
                            return false;
                        }
                        AggregateResult partial{};
                        if (!predicates.empty()) {
                            Mask mask;
                            if (!BuildPredicateMask(segment.Fields(), predicates, &mask)) {
                                return false;
                            }
                            AggregateMixed(segment.Fields()[field_index], &mask, &partial);
                        } else {
                            AggregateMixed(segment.Fields()[field_index], nullptr, &partial);
                        }
                        MergeAggregate(partial, &result);
                        return true;
                    })) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
        }

        const uint64_t rows_scanned = dataset.RowCount();
        std::vector<uint8_t> out(sizeof(uint64_t) + sizeof(double) * 3 + sizeof(uint8_t) +
                                 sizeof(uint64_t));
        size_t out_cursor = 0;
        std::memcpy(out.data() + out_cursor, &result.count, sizeof(uint64_t));
        out_cursor += sizeof(uint64_t);
        std::memcpy(out.data() + out_cursor, &result.sum, sizeof(double));
        out_cursor += sizeof(double);
        std::memcpy(out.data() + out_cursor, &result.min, sizeof(double));
        out_cursor += sizeof(double);
        std::memcpy(out.data() + out_cursor, &result.max, sizeof(double));
        out_cursor += sizeof(double);
        const uint8_t has_value = result.has_value ? 1 : 0;
        std::memcpy(out.data() + out_cursor, &has_value, sizeof(uint8_t));
        out_cursor += sizeof(uint8_t);
        std::memcpy(out.data() + out_cursor, &rows_scanned, sizeof(uint64_t));

        SendStatus(client, header, Status::kOk, out);
    }

    void HandleVectorSearch(int client, const MessageHeader& header,
                            const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string db_name;
        std::string name;
        uint16_t field_index = 0;
        uint8_t metric_id = 0;
        uint32_t dimension = 0;
        uint32_t top_k = 0;
        uint16_t predicate_count = 0;
        if (!ReadName(payload, &cursor, &db_name) || !ReadName(payload, &cursor, &name) ||
            !ReadScalar(payload, &cursor, &field_index) ||
            !ReadScalar(payload, &cursor, &metric_id) ||
            !ReadScalar(payload, &cursor, &dimension) || !ReadScalar(payload, &cursor, &top_k) ||
            !ReadScalar(payload, &cursor, &predicate_count) ||
            dimension == 0 || top_k == 0 || metric_id > 2 ||
            predicate_count > 1024) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::vector<VectorSearchPredicate> predicates;
        predicates.reserve(predicate_count);
        for (uint16_t i = 0; i < predicate_count; ++i) {
            uint16_t pred_field = 0;
            uint8_t pred_op = 0;
            double pred_value = 0.0;
            if (!ReadScalar(payload, &cursor, &pred_field) ||
                !ReadScalar(payload, &cursor, &pred_op) || pred_op > 5 ||
                !ReadScalar(payload, &cursor, &pred_value)) {
                SendStatus(client, header, Status::kBadRequest, {}); return;
            }
            predicates.push_back({pred_field, DecodeCompareOp(pred_op), pred_value});
        }
        if (dimension > (payload.size() - cursor) / sizeof(float) ||
            cursor + static_cast<size_t>(dimension) * sizeof(float) != payload.size()) {
            SendStatus(client, header, Status::kBadRequest, {}); return;
        }
        auto db_it = databases_.find(db_name);
        if (db_it == databases_.end()) { SendStatus(client, header, Status::kNotFound, {}); return; }
        auto it = db_it->second.datasets.find(name);
        if (it == db_it->second.datasets.end()) { SendStatus(client, header, Status::kNotFound, {}); return; }
        std::vector<float> query(dimension);
        std::memcpy(query.data(), payload.data() + cursor, query.size() * sizeof(float));
        std::vector<VectorSearchHit> hits;
        if (!VectorSearch(*it->second.dataset, field_index, query.data(), query.size(), top_k,
                          static_cast<VectorMetric>(metric_id), &hits, predicates)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::vector<uint8_t> response(sizeof(uint32_t) + hits.size() * 12);
        const uint32_t count = static_cast<uint32_t>(hits.size());
        std::memcpy(response.data(), &count, sizeof(count));
        size_t out_cursor = sizeof(count);
        for (const auto& hit : hits) {
            std::memcpy(response.data() + out_cursor, &hit.row_id, sizeof(hit.row_id));
            out_cursor += sizeof(hit.row_id);
            std::memcpy(response.data() + out_cursor, &hit.distance, sizeof(hit.distance));
            out_cursor += sizeof(hit.distance);
        }
        SendStatus(client, header, Status::kOk, response);
    }

    void HandleScan(int client, const MessageHeader& header,
                    const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string db_name;
        if (!ReadName(payload, &cursor, &db_name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::string name;
        if (!ReadName(payload, &cursor, &name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        auto db_it = databases_.find(db_name);
        if (db_it == databases_.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        auto it = db_it->second.datasets.find(name);
        if (it == db_it->second.datasets.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        uint16_t column_count = 0;
        if (!ReadScalar(payload, &cursor, &column_count)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::vector<uint16_t> column_indices;
        column_indices.reserve(column_count);
        for (uint16_t i = 0; i < column_count; ++i) {
            uint16_t column_index = 0;
            if (!ReadScalar(payload, &cursor, &column_index)) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            column_indices.push_back(column_index);
        }
        uint16_t predicate_count = 0;
        if (!ReadPredicateCount(payload, &cursor, &predicate_count)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::vector<PredicateSpec> predicates;
        predicates.reserve(predicate_count);
        for (uint16_t i = 0; i < predicate_count; ++i) {
            uint16_t pred_field = 0;
            uint8_t op = 0;
            double value = 0.0;
            if (!ReadScalar(payload, &cursor, &pred_field) ||
                !ReadScalar(payload, &cursor, &op) ||
                !ReadScalar(payload, &cursor, &value)) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            predicates.push_back(PredicateSpec{pred_field, DecodeCompareOp(op), value});
        }
        uint64_t limit = 0;
        uint64_t offset = 0;
        if (!ReadScalar(payload, &cursor, &limit) ||
            !ReadScalar(payload, &cursor, &offset)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }

        const Dataset& dataset = *it->second.dataset;
        if (dataset.Fields().empty()) {
            SendStatus(client, header, Status::kOk, {});
            return;
        }
        if (column_indices.empty()) {
            column_indices.reserve(dataset.Fields().size());
            for (size_t idx = 0; idx < dataset.Fields().size(); ++idx) {
                column_indices.push_back(static_cast<uint16_t>(idx));
            }
        }
        for (uint16_t idx : column_indices) {
            if (idx >= dataset.Fields().size()) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
        }

        struct OutputColumn {
            uint16_t index = 0;
            FieldType type = FieldType::kInt32;
            std::vector<uint8_t> data;
            std::vector<uint32_t> lengths;
            std::vector<uint8_t> validity;
            bool has_null = false;
        };

        struct ScanCompressionMetrics {
            uint64_t raw_bytes = 0;
            uint64_t decode_bytes = 0;
            uint64_t compressed_columns_touched = 0;
            uint64_t compressed_columns_skipped = 0;
            uint64_t decode_ns = 0;
            uint64_t predicate_ns = 0;
            uint64_t output_ns = 0;
        };

        ScanCompressionMetrics scan_metrics;
        auto now = []() { return std::chrono::steady_clock::now(); };
        auto add_ns = [](uint64_t* target,
                         const std::chrono::steady_clock::time_point& start,
                         const std::chrono::steady_clock::time_point& end) {
            if (!target) {
                return;
            }
            *target += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        };

        std::vector<OutputColumn> outputs;
        outputs.reserve(column_indices.size());
        for (uint16_t idx : column_indices) {
            OutputColumn out;
            out.index = idx;
            out.type = dataset.Fields()[idx].Type();
            outputs.push_back(std::move(out));
        }
        size_t target_rows = dataset.RowCount();
        if (offset >= target_rows) {
            target_rows = 0;
        } else {
            target_rows -= static_cast<size_t>(offset);
        }
        if (limit > 0 && limit < target_rows) {
            target_rows = static_cast<size_t>(limit);
        }
        for (auto& out : outputs) {
            out.validity.reserve(target_rows);
            if (out.type == FieldType::kString || out.type == FieldType::kBytes) {
                out.lengths.reserve(target_rows);
            } else {
                size_t elem_size = 0;
                switch (out.type) {
                    case FieldType::kInt32:
                        elem_size = sizeof(int32_t);
                        break;
                    case FieldType::kInt64:
                        elem_size = sizeof(int64_t);
                        break;
                    case FieldType::kFloat64:
                        elem_size = sizeof(double);
                        break;
                    case FieldType::kBool:
                        elem_size = sizeof(uint8_t);
                        break;
                    case FieldType::kDictInt32:
                        elem_size = sizeof(int32_t);
                        break;
                    case FieldType::kString:
                    case FieldType::kBytes:
                    case FieldType::kArray:
                    case FieldType::kObject:
                        elem_size = 0;
                        break;
                }
                if (elem_size > 0 && target_rows > 0) {
                    out.data.reserve(target_rows * elem_size);
                }
            }
        }

        auto append_value = [](OutputColumn& out, const FieldVector& field, size_t row) {
            auto append_bytes = [&](const void* src, size_t size) {
                const size_t offset = out.data.size();
                out.data.resize(offset + size);
                std::memcpy(out.data.data() + offset, src, size);
            };
            if (field.HasNulls() && !field.IsValid(row)) {
                out.has_null = true;
                out.validity.push_back(0);
                switch (field.Type()) {
                    case FieldType::kInt32: {
                        int32_t val = 0;
                        append_bytes(&val, sizeof(val));
                        break;
                    }
                    case FieldType::kInt64: {
                        int64_t val = 0;
                        append_bytes(&val, sizeof(val));
                        break;
                    }
                    case FieldType::kFloat64: {
                        double val = 0.0;
                        append_bytes(&val, sizeof(val));
                        break;
                    }
                    case FieldType::kBool: {
                        uint8_t val = 0;
                        append_bytes(&val, sizeof(val));
                        break;
                    }
                    case FieldType::kDictInt32: {
                        int32_t val = 0;
                        append_bytes(&val, sizeof(val));
                        break;
                    }
                    case FieldType::kString:
                    case FieldType::kBytes: {
                        out.lengths.push_back(0);
                        break;
                    }
                    case FieldType::kArray:
                    case FieldType::kObject: {
                        out.lengths.push_back(0);
                        break;
                    }
                }
                return;
            }

            out.validity.push_back(1);
            switch (field.Type()) {
                case FieldType::kInt32: {
                    int32_t val = field.DataInt32()[row];
                    append_bytes(&val, sizeof(val));
                    break;
                }
                case FieldType::kInt64: {
                    int64_t val = field.DataInt64()[row];
                    append_bytes(&val, sizeof(val));
                    break;
                }
                case FieldType::kFloat64: {
                    double val = field.DataFloat64()[row];
                    append_bytes(&val, sizeof(val));
                    break;
                }
                case FieldType::kBool: {
                    uint8_t val = field.DataBool()[row] ? 1 : 0;
                    append_bytes(&val, sizeof(val));
                    break;
                }
                case FieldType::kDictInt32: {
                    int32_t val = field.DictionaryValue(field.DataDictIds()[row]);
                    append_bytes(&val, sizeof(val));
                    break;
                }
                case FieldType::kString:
                case FieldType::kBytes: {
                    const auto* lengths = field.DataLengths();
                    const auto* bytes = field.DataBytes();
                    uint64_t offset = 0;
                    for (size_t i = 0; i < row; ++i) {
                        offset += lengths[i];
                    }
                    const uint32_t len = lengths[row];
                    out.lengths.push_back(len);
                    if (len > 0) {
                        out.data.insert(out.data.end(), bytes + offset, bytes + offset + len);
                    }
                    break;
                }
                case FieldType::kArray:
                case FieldType::kObject: {
                    out.lengths.push_back(0);
                    break;
                }
            }
        };

        struct VarlenState {
            const uint32_t* lengths = nullptr;
            const uint8_t* bytes = nullptr;
            uint64_t offset = 0;
        };

        struct DecodedColumns {
            std::vector<CompressedColumnView> views;
            std::vector<std::vector<uint8_t>> data;
            std::vector<std::vector<uint8_t>> aux;
        };

        struct Lz4LiteralStream {
            const uint8_t* src = nullptr;
            size_t src_size = 0;
            size_t src_offset = 0;
            const uint8_t* lit_ptr = nullptr;
            size_t lit_remaining = 0;

            bool Init(const uint8_t* data, size_t size) {
                src = data;
                src_size = size;
                src_offset = 0;
                lit_ptr = nullptr;
                lit_remaining = 0;
                return true;
            }

            bool Read(uint8_t* dst, size_t size) {
                if (size == 0) {
                    return true;
                }
                if (!dst || !src) {
                    return false;
                }
                size_t out = 0;
                while (out < size) {
                    if (lit_remaining == 0) {
                        if (src_offset >= src_size) {
                            return false;
                        }
                        const uint8_t token = src[src_offset++];
                        size_t literal_len = token >> 4;
                        if (literal_len == 15) {
                            while (src_offset < src_size) {
                                const uint8_t byte = src[src_offset++];
                                literal_len += byte;
                                if (byte != 255) {
                                    break;
                                }
                            }
                        }
                        if (src_offset + literal_len > src_size) {
                            return false;
                        }
                        lit_ptr = src + src_offset;
                        lit_remaining = literal_len;
                        src_offset += literal_len;
                    }
                    const size_t take = std::min(lit_remaining, size - out);
                    std::memcpy(dst + out, lit_ptr, take);
                    lit_ptr += take;
                    lit_remaining -= take;
                    out += take;
                }
                return true;
            }
        };

        auto build_readable_columns = [&](const std::vector<CompressedColumnView>& columns,
                                          const std::vector<uint8_t>& decode_needed,
                                          DecodedColumns* decoded,
                                          std::vector<CompressedColumnView>* out) -> bool {
            if (!decoded || !out) {
                return false;
            }
            out->clear();
            out->reserve(columns.size());
            if (decoded->data.size() < columns.size()) {
                decoded->data.resize(columns.size());
            }
            if (decoded->aux.size() < columns.size()) {
                decoded->aux.resize(columns.size());
            }
            for (size_t i = 0; i < columns.size(); ++i) {
                decoded->data[i].clear();
                decoded->aux[i].clear();
                const auto& col = columns[i];
                if (col.kind == ColumnCompressionKind::kLz4 && decode_needed[i]) {
                    if (col.raw_data_size > 0) {
                        decoded->data[i].resize(col.raw_data_size);
                        if (!DecodeLz4Literal(col.data, col.data_size,
                                              decoded->data[i].data(),
                                              decoded->data[i].size())) {
                            return false;
                        }
                    }
                    if (col.raw_aux_size > 0) {
                        decoded->aux[i].resize(col.raw_aux_size);
                        if (!DecodeLz4Literal(col.aux, col.aux_size,
                                              decoded->aux[i].data(),
                                              decoded->aux[i].size())) {
                            return false;
                        }
                    }
                    CompressedColumnView view = col;
                    view.kind = ColumnCompressionKind::kNone;
                    view.ops = DefaultCompressionOps();
                    view.data = decoded->data[i].empty() ? nullptr : decoded->data[i].data();
                    view.data_size = col.raw_data_size;
                    view.aux = decoded->aux[i].empty() ? nullptr : decoded->aux[i].data();
                    view.aux_size = col.raw_aux_size;
                    out->push_back(view);
                } else {
                    out->push_back(col);
                }
            }
            return true;
        };

        auto append_value_compressed = [](OutputColumn& out, const CompressedColumnView& col,
                                          size_t row, VarlenState* varlen_state) {
            auto append_bytes = [&](const void* src, size_t size) {
                const size_t offset = out.data.size();
                out.data.resize(offset + size);
                std::memcpy(out.data.data() + offset, src, size);
            };
            if (!IsValid(col, row)) {
                out.has_null = true;
                out.validity.push_back(0);
                switch (col.type) {
                    case FieldType::kInt32: {
                        int32_t val = 0;
                        append_bytes(&val, sizeof(val));
                        break;
                    }
                    case FieldType::kInt64: {
                        int64_t val = 0;
                        append_bytes(&val, sizeof(val));
                        break;
                    }
                    case FieldType::kFloat64: {
                        double val = 0.0;
                        append_bytes(&val, sizeof(val));
                        break;
                    }
                    case FieldType::kBool: {
                        uint8_t val = 0;
                        append_bytes(&val, sizeof(val));
                        break;
                    }
                    case FieldType::kDictInt32: {
                        int32_t val = 0;
                        append_bytes(&val, sizeof(val));
                        break;
                    }
                    case FieldType::kString:
                    case FieldType::kBytes:
                        out.lengths.push_back(0);
                        break;
                    case FieldType::kArray:
                    case FieldType::kObject:
                        break;
                }
                return;
            }
            out.validity.push_back(1);
            switch (col.type) {
                case FieldType::kInt32: {
                    int64_t val = 0;
                    if (ReadInt64Value(col, row, &val)) {
                        const int32_t out_val = static_cast<int32_t>(val);
                        append_bytes(&out_val, sizeof(out_val));
                    }
                    break;
                }
                case FieldType::kInt64: {
                    int64_t val = 0;
                    if (ReadInt64Value(col, row, &val)) {
                        append_bytes(&val, sizeof(val));
                    }
                    break;
                }
                case FieldType::kFloat64: {
                    double val = 0.0;
                    if (ReadNumericValue(col, row, &val)) {
                        append_bytes(&val, sizeof(val));
                    }
                    break;
                }
                case FieldType::kBool: {
                    int64_t val = 0;
                    if (ReadInt64Value(col, row, &val)) {
                        const uint8_t out_val = val != 0 ? 1 : 0;
                        append_bytes(&out_val, sizeof(out_val));
                    }
                    break;
                }
                case FieldType::kDictInt32: {
                    int64_t val = 0;
                    if (ReadInt64Value(col, row, &val)) {
                        const int32_t out_val = static_cast<int32_t>(val);
                        append_bytes(&out_val, sizeof(out_val));
                    }
                    break;
                }
                case FieldType::kString:
                case FieldType::kBytes: {
                    if (!varlen_state || !varlen_state->lengths || !varlen_state->bytes) {
                        out.lengths.push_back(0);
                        break;
                    }
                    const uint32_t len = varlen_state->lengths[row];
                    const uint64_t offset = varlen_state->offset;
                    varlen_state->offset += len;
                    out.lengths.push_back(len);
                    if (len > 0) {
                        out.data.insert(out.data.end(), varlen_state->bytes + offset,
                                        varlen_state->bytes + offset + len);
                    }
                    break;
                }
                case FieldType::kArray:
                case FieldType::kObject:
                    break;
            }
        };

        uint64_t remaining_limit = limit;
        uint64_t remaining_offset = offset;
        bool stop = false;
        static thread_local DecodedColumns decoded_cache;
        static thread_local std::vector<CompressedColumnView> readable_cache;

        auto scan_fields = [&](const std::vector<FieldVector>& fields, size_t row_count) -> bool {
            Mask mask;
            if (!predicates.empty()) {
                if (!BuildPredicateMask(fields, predicates, &mask)) {
                    return false;
                }
            } else {
                mask.Resize(row_count);
                for (size_t i = 0; i < row_count; ++i) {
                    mask.Set(i, true);
                }
            }
            for (size_t row = 0; row < row_count; ++row) {
                if (!mask.Get(row)) {
                    continue;
                }
                if (remaining_offset > 0) {
                    remaining_offset -= 1;
                    continue;
                }
                for (auto& out : outputs) {
                    append_value(out, fields[out.index], row);
                }
                if (remaining_limit > 0) {
                    remaining_limit -= 1;
                    if (remaining_limit == 0) {
                        stop = true;
                        return true;
                    }
                }
            }
            return true;
        };

        auto scan_compressed = [&](const std::vector<CompressedColumnView>& columns,
                                   size_t row_count) -> bool {
            Mask mask;
            if (!predicates.empty()) {
                const auto pred_start = now();
                bool has_mask = false;
                Mask combined;
                for (const auto& pred : predicates) {
                    if (pred.field_index >= columns.size()) {
                        return false;
                    }
                    const auto& col = columns[pred.field_index];
                    CompressionPredicate cpred;
                    cpred.type = col.type;
                    cpred.op = pred.op;
                    cpred.i64 = static_cast<int64_t>(pred.value);
                    cpred.f64 = pred.value;
                    Mask current;
                    if (!BuildMaskCompressed(col, cpred, &current)) {
                        return false;
                    }
                    if (!has_mask) {
                        combined = std::move(current);
                        has_mask = true;
                    } else {
                        combined = Mask::And(combined, current);
                    }
                }
                mask = std::move(combined);
                if (compression_metrics_) {
                    add_ns(&scan_metrics.predicate_ns, pred_start, now());
                }
            } else {
                mask.Resize(row_count);
                for (size_t i = 0; i < row_count; ++i) {
                    mask.Set(i, true);
                }
            }

            const auto output_start = now();
            std::vector<VarlenState> varlen_states(outputs.size());
            for (size_t i = 0; i < outputs.size(); ++i) {
                const auto& col = columns[outputs[i].index];
                if (col.type == FieldType::kString || col.type == FieldType::kBytes) {
                    varlen_states[i].lengths =
                        reinterpret_cast<const uint32_t*>(col.aux);
                    varlen_states[i].bytes = col.data;
                    varlen_states[i].offset = 0;
                }
            }

            constexpr size_t kBatchRows = 4096;
            for (size_t base = 0; base < row_count; base += kBatchRows) {
                const size_t end = std::min(row_count, base + kBatchRows);
                for (size_t row = base; row < end; ++row) {
                    const bool keep = mask.Get(row);
                    if (!keep) {
                        for (size_t i = 0; i < outputs.size(); ++i) {
                            if (outputs[i].type == FieldType::kString ||
                                outputs[i].type == FieldType::kBytes) {
                                if (varlen_states[i].lengths) {
                                    varlen_states[i].offset += varlen_states[i].lengths[row];
                                }
                            }
                        }
                        continue;
                    }
                    if (remaining_offset > 0) {
                        remaining_offset -= 1;
                        for (size_t i = 0; i < outputs.size(); ++i) {
                            if (outputs[i].type == FieldType::kString ||
                                outputs[i].type == FieldType::kBytes) {
                                if (varlen_states[i].lengths) {
                                    varlen_states[i].offset += varlen_states[i].lengths[row];
                                }
                            }
                        }
                        continue;
                    }
                    for (size_t i = 0; i < outputs.size(); ++i) {
                        VarlenState* state_ptr = nullptr;
                        if (outputs[i].type == FieldType::kString ||
                            outputs[i].type == FieldType::kBytes) {
                            state_ptr = &varlen_states[i];
                        }
                        append_value_compressed(outputs[i], columns[outputs[i].index], row,
                                                state_ptr);
                    }
                    if (remaining_limit > 0) {
                        remaining_limit -= 1;
                        if (remaining_limit == 0) {
                            stop = true;
                            return true;
                        }
                    }
                }
                if (stop) {
                    return true;
                }
            }
            if (compression_metrics_) {
                add_ns(&scan_metrics.output_ns, output_start, now());
            }
            return true;
        };

        auto segment_matches = [&](const Segment& segment) -> bool {
            if (predicates.empty()) {
                return true;
            }
            const auto& stats = segment.ColumnStats();
            if (stats.size() != segment.Fields().size()) {
                return true;
            }
            for (const auto& pred : predicates) {
                if (pred.field_index >= stats.size()) {
                    return false;
                }
                if (!SegmentMatchesPredicate(stats[pred.field_index], pred.op, pred.value)) {
                    return false;
                }
            }
            return true;
        };

        auto scan_compressed_lz4 = [&](const std::vector<CompressedColumnView>& columns,
                                       const std::vector<uint8_t>& decode_needed,
                                       size_t row_count) -> bool {
            if (columns.empty()) {
                return true;
            }
            std::vector<Lz4LiteralStream> data_streams(columns.size());
            std::vector<Lz4LiteralStream> aux_streams(columns.size());
            for (size_t i = 0; i < columns.size(); ++i) {
                if (columns[i].kind != ColumnCompressionKind::kLz4 || !decode_needed[i]) {
                    continue;
                }
                if (columns[i].data_size > 0) {
                    data_streams[i].Init(columns[i].data, columns[i].data_size);
                }
                if (columns[i].aux_size > 0) {
                    aux_streams[i].Init(columns[i].aux, columns[i].aux_size);
                }
            }
            if (decoded_cache.data.size() < columns.size()) {
                decoded_cache.data.resize(columns.size());
            }
            if (decoded_cache.aux.size() < columns.size()) {
                decoded_cache.aux.resize(columns.size());
            }

            constexpr size_t kBatchRows = 4096;
            for (size_t base = 0; base < row_count; base += kBatchRows) {
                const size_t end = std::min(row_count, base + kBatchRows);
                const size_t batch_rows = end - base;
                for (size_t i = 0; i < columns.size(); ++i) {
                    if (columns[i].kind != ColumnCompressionKind::kLz4 || !decode_needed[i]) {
                        continue;
                    }
                    const auto decode_start = now();
                    decoded_cache.data[i].clear();
                    decoded_cache.aux[i].clear();
                    switch (columns[i].type) {
                        case FieldType::kInt32: {
                            const size_t bytes = batch_rows * sizeof(int32_t);
                            decoded_cache.data[i].resize(bytes);
                            if (!data_streams[i].Read(decoded_cache.data[i].data(), bytes)) {
                                return false;
                            }
                            if (compression_metrics_) {
                                scan_metrics.decode_bytes += bytes;
                            }
                            break;
                        }
                        case FieldType::kInt64: {
                            const size_t bytes = batch_rows * sizeof(int64_t);
                            decoded_cache.data[i].resize(bytes);
                            if (!data_streams[i].Read(decoded_cache.data[i].data(), bytes)) {
                                return false;
                            }
                            if (compression_metrics_) {
                                scan_metrics.decode_bytes += bytes;
                            }
                            break;
                        }
                        case FieldType::kFloat64: {
                            const size_t bytes = batch_rows * sizeof(double);
                            decoded_cache.data[i].resize(bytes);
                            if (!data_streams[i].Read(decoded_cache.data[i].data(), bytes)) {
                                return false;
                            }
                            if (compression_metrics_) {
                                scan_metrics.decode_bytes += bytes;
                            }
                            break;
                        }
                        case FieldType::kBool: {
                            const size_t bytes = batch_rows * sizeof(uint8_t);
                            decoded_cache.data[i].resize(bytes);
                            if (!data_streams[i].Read(decoded_cache.data[i].data(), bytes)) {
                                return false;
                            }
                            if (compression_metrics_) {
                                scan_metrics.decode_bytes += bytes;
                            }
                            break;
                        }
                        case FieldType::kDictInt32: {
                            const size_t bytes = batch_rows * sizeof(uint32_t);
                            decoded_cache.data[i].resize(bytes);
                            if (!data_streams[i].Read(decoded_cache.data[i].data(), bytes)) {
                                return false;
                            }
                            if (compression_metrics_) {
                                scan_metrics.decode_bytes += bytes;
                            }
                            break;
                        }
                        case FieldType::kString:
                        case FieldType::kBytes: {
                            const size_t len_bytes = batch_rows * sizeof(uint32_t);
                            decoded_cache.aux[i].resize(len_bytes);
                            if (!aux_streams[i].Read(decoded_cache.aux[i].data(), len_bytes)) {
                                return false;
                            }
                            if (compression_metrics_) {
                                scan_metrics.decode_bytes += len_bytes;
                            }
                            const auto* lengths =
                                reinterpret_cast<const uint32_t*>(decoded_cache.aux[i].data());
                            size_t bytes_total = 0;
                            for (size_t r = 0; r < batch_rows; ++r) {
                                bytes_total += lengths[r];
                            }
                            decoded_cache.data[i].resize(bytes_total);
                            if (bytes_total > 0 &&
                                !data_streams[i].Read(decoded_cache.data[i].data(),
                                                      bytes_total)) {
                                return false;
                            }
                            if (compression_metrics_) {
                                scan_metrics.decode_bytes += bytes_total;
                            }
                            break;
                        }
                        case FieldType::kArray:
                        case FieldType::kObject:
                            break;
                    }
                    if (compression_metrics_) {
                        add_ns(&scan_metrics.decode_ns, decode_start, now());
                    }
                }

                auto read_int64_at = [&](size_t col_index, size_t row, int64_t* out) -> bool {
                    const auto& col = columns[col_index];
                    if (col.kind == ColumnCompressionKind::kLz4 && decode_needed[col_index]) {
                        const size_t rel = row - base;
                        const auto& buf = decoded_cache.data[col_index];
                        switch (col.type) {
                            case FieldType::kInt32: {
                                int32_t v = 0;
                                std::memcpy(&v, buf.data() + rel * sizeof(int32_t), sizeof(int32_t));
                                *out = v;
                                return true;
                            }
                            case FieldType::kInt64: {
                                int64_t v = 0;
                                std::memcpy(&v, buf.data() + rel * sizeof(int64_t), sizeof(int64_t));
                                *out = v;
                                return true;
                            }
                            case FieldType::kBool: {
                                uint8_t v = 0;
                                std::memcpy(&v, buf.data() + rel, sizeof(uint8_t));
                                *out = v ? 1 : 0;
                                return true;
                            }
                            case FieldType::kDictInt32: {
                                uint32_t id = 0;
                                std::memcpy(&id, buf.data() + rel * sizeof(uint32_t),
                                            sizeof(uint32_t));
                                if (col.dict) {
                                    *out = static_cast<int64_t>(col.dict->Value(id));
                                } else {
                                    *out = static_cast<int64_t>(id);
                                }
                                return true;
                            }
                            default:
                                return false;
                        }
                    }
                    return ReadInt64Value(col, row, out);
                };

                auto read_double_at = [&](size_t col_index, size_t row, double* out) -> bool {
                    const auto& col = columns[col_index];
                    if (col.kind == ColumnCompressionKind::kLz4 && decode_needed[col_index]) {
                        const size_t rel = row - base;
                        const auto& buf = decoded_cache.data[col_index];
                        switch (col.type) {
                            case FieldType::kFloat64: {
                                double v = 0.0;
                                std::memcpy(&v, buf.data() + rel * sizeof(double), sizeof(double));
                                *out = v;
                                return true;
                            }
                            case FieldType::kInt32:
                            case FieldType::kInt64:
                            case FieldType::kBool:
                            case FieldType::kDictInt32: {
                                int64_t v = 0;
                                if (!read_int64_at(col_index, row, &v)) {
                                    return false;
                                }
                                *out = static_cast<double>(v);
                                return true;
                            }
                            default:
                                return false;
                        }
                    }
                    return ReadNumericValue(col, row, out);
                };

                std::vector<VarlenState> varlen_states(outputs.size());
                for (size_t i = 0; i < outputs.size(); ++i) {
                    const auto& col = columns[outputs[i].index];
                    if (col.type == FieldType::kString || col.type == FieldType::kBytes) {
                        if (col.kind == ColumnCompressionKind::kLz4) {
                            varlen_states[i].lengths = reinterpret_cast<const uint32_t*>(
                                decoded_cache.aux[outputs[i].index].data());
                            varlen_states[i].bytes = decoded_cache.data[outputs[i].index].data();
                        } else {
                            varlen_states[i].lengths =
                                reinterpret_cast<const uint32_t*>(col.aux);
                            varlen_states[i].bytes = col.data;
                        }
                        varlen_states[i].offset = 0;
                    }
                }

                const auto pred_start = now();
                for (size_t row = base; row < end; ++row) {
                    bool keep = true;
                    for (const auto& pred : predicates) {
                        if (!IsValid(columns[pred.field_index], row)) {
                            keep = false;
                            break;
                        }
                        const auto type = columns[pred.field_index].type;
                        if (type == FieldType::kFloat64) {
                            double value = 0.0;
                            if (!read_double_at(pred.field_index, row, &value)) {
                                keep = false;
                                break;
                            }
                            const uint8_t ok =
                                CompareFloat64Branchless(value, pred.value, pred.op);
                            if (!ok) {
                                keep = false;
                                break;
                            }
                        } else {
                            int64_t value = 0;
                            if (!read_int64_at(pred.field_index, row, &value)) {
                                keep = false;
                                break;
                            }
                            const uint8_t ok =
                                CompareInt64Branchless(value,
                                                       static_cast<int64_t>(pred.value),
                                                       pred.op);
                            if (!ok) {
                                keep = false;
                                break;
                            }
                        }
                    }
                    if (!keep) {
                        for (size_t i = 0; i < outputs.size(); ++i) {
                            if (outputs[i].type == FieldType::kString ||
                                outputs[i].type == FieldType::kBytes) {
                                if (varlen_states[i].lengths) {
                                    const size_t rel = row - base;
                                    varlen_states[i].offset += varlen_states[i].lengths[rel];
                                }
                            }
                        }
                        continue;
                    }
                    if (remaining_offset > 0) {
                        remaining_offset -= 1;
                        for (size_t i = 0; i < outputs.size(); ++i) {
                            if (outputs[i].type == FieldType::kString ||
                                outputs[i].type == FieldType::kBytes) {
                                if (varlen_states[i].lengths) {
                                    const size_t rel = row - base;
                                    varlen_states[i].offset += varlen_states[i].lengths[rel];
                                }
                            }
                        }
                        continue;
                    }
                    for (size_t i = 0; i < outputs.size(); ++i) {
                        VarlenState* state_ptr = nullptr;
                        if (outputs[i].type == FieldType::kString ||
                            outputs[i].type == FieldType::kBytes) {
                            state_ptr = &varlen_states[i];
                        }
                        if (columns[outputs[i].index].kind == ColumnCompressionKind::kLz4 &&
                            decode_needed[outputs[i].index]) {
                            const size_t rel = row - base;
                            const auto& col = columns[outputs[i].index];
                            if (!IsValid(col, row)) {
                                outputs[i].has_null = true;
                                outputs[i].validity.push_back(0);
                                if (col.type == FieldType::kString ||
                                    col.type == FieldType::kBytes) {
                                    outputs[i].lengths.push_back(0);
                                } else {
                                    const uint64_t zero = 0;
                                    outputs[i].data.insert(outputs[i].data.end(),
                                                           reinterpret_cast<const uint8_t*>(&zero),
                                                           reinterpret_cast<const uint8_t*>(&zero) +
                                                               (col.type == FieldType::kInt32
                                                                    ? sizeof(int32_t)
                                                                    : col.type == FieldType::kInt64
                                                                          ? sizeof(int64_t)
                                                                          : col.type ==
                                                                                    FieldType::kFloat64
                                                                                ? sizeof(double)
                                                                                : sizeof(uint8_t)));
                                }
                                continue;
                            }
                            outputs[i].validity.push_back(1);
                            switch (col.type) {
                                case FieldType::kInt32: {
                                    int32_t v = 0;
                                    std::memcpy(&v,
                                                decoded_cache.data[outputs[i].index].data() +
                                                    rel * sizeof(int32_t),
                                                sizeof(int32_t));
                                    outputs[i].data.insert(
                                        outputs[i].data.end(),
                                        reinterpret_cast<const uint8_t*>(&v),
                                        reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
                                    break;
                                }
                                case FieldType::kInt64: {
                                    int64_t v = 0;
                                    std::memcpy(&v,
                                                decoded_cache.data[outputs[i].index].data() +
                                                    rel * sizeof(int64_t),
                                                sizeof(int64_t));
                                    outputs[i].data.insert(
                                        outputs[i].data.end(),
                                        reinterpret_cast<const uint8_t*>(&v),
                                        reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
                                    break;
                                }
                                case FieldType::kFloat64: {
                                    double v = 0.0;
                                    std::memcpy(&v,
                                                decoded_cache.data[outputs[i].index].data() +
                                                    rel * sizeof(double),
                                                sizeof(double));
                                    outputs[i].data.insert(
                                        outputs[i].data.end(),
                                        reinterpret_cast<const uint8_t*>(&v),
                                        reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
                                    break;
                                }
                                case FieldType::kBool: {
                                    uint8_t v = decoded_cache.data[outputs[i].index][rel] ? 1 : 0;
                                    outputs[i].data.push_back(v);
                                    break;
                                }
                                case FieldType::kDictInt32: {
                                    uint32_t id = 0;
                                    std::memcpy(&id,
                                                decoded_cache.data[outputs[i].index].data() +
                                                    rel * sizeof(uint32_t),
                                                sizeof(uint32_t));
                                    int32_t v = static_cast<int32_t>(
                                        columns[outputs[i].index].dict
                                            ? columns[outputs[i].index].dict->Value(id)
                                            : id);
                                    outputs[i].data.insert(
                                        outputs[i].data.end(),
                                        reinterpret_cast<const uint8_t*>(&v),
                                        reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
                                    break;
                                }
                                case FieldType::kString:
                                case FieldType::kBytes: {
                                    if (!state_ptr || !state_ptr->lengths ||
                                        !state_ptr->bytes) {
                                        outputs[i].lengths.push_back(0);
                                        break;
                                    }
                                    const uint32_t len = state_ptr->lengths[rel];
                                    const uint64_t offset = state_ptr->offset;
                                    state_ptr->offset += len;
                                    outputs[i].lengths.push_back(len);
                                    if (len > 0) {
                                        outputs[i].data.insert(outputs[i].data.end(),
                                                               state_ptr->bytes + offset,
                                                               state_ptr->bytes + offset + len);
                                    }
                                    break;
                                }
                                case FieldType::kArray:
                                case FieldType::kObject:
                                    break;
                            }
                        } else {
                            append_value_compressed(outputs[i], columns[outputs[i].index], row,
                                                    state_ptr);
                        }
                    }
                    if (remaining_limit > 0) {
                        remaining_limit -= 1;
                        if (remaining_limit == 0) {
                            stop = true;
                            return true;
                        }
                    }
                }
                if (compression_metrics_) {
                    add_ns(&scan_metrics.predicate_ns, pred_start, now());
                }
                if (stop) {
                    return true;
                }
            }
            return true;
        };

        if (!ForEachSegment(
                db_name, it->second,
                [&](const Segment& segment) -> bool {
                    if (stop) {
                        return true;
                    }
                    if (!segment_matches(segment)) {
                        if (compression_metrics_ && segment.IsSealed()) {
                            const auto& kinds = segment.CompressionKinds();
                            for (const auto kind : kinds) {
                                if (kind != ColumnCompressionKind::kNone) {
                                    scan_metrics.compressed_columns_skipped += 1;
                                }
                            }
                        }
                        return true;
                    }
                    if (segment.IsSealed() && !segment.CompressedColumns().empty()) {
                        const auto& columns = segment.CompressedColumns();
                        std::vector<uint8_t> decode_needed(columns.size(), 0);
                        for (const auto& pred : predicates) {
                            if (pred.field_index < decode_needed.size()) {
                                decode_needed[pred.field_index] = 1;
                            }
                        }
                        for (const auto& out : outputs) {
                            if (out.index < decode_needed.size()) {
                                decode_needed[out.index] = 1;
                            }
                        }
                        if (compression_metrics_) {
                            for (size_t i = 0; i < columns.size(); ++i) {
                                if (!decode_needed[i]) {
                                    continue;
                                }
                                scan_metrics.raw_bytes +=
                                    columns[i].raw_data_size + columns[i].raw_aux_size;
                                if (columns[i].kind != ColumnCompressionKind::kNone) {
                                    scan_metrics.compressed_columns_touched += 1;
                                }
                            }
                        }
                        bool has_lz4 = false;
                        for (size_t i = 0; i < columns.size(); ++i) {
                            if (decode_needed[i] &&
                                columns[i].kind == ColumnCompressionKind::kLz4) {
                                has_lz4 = true;
                                break;
                            }
                        }
                        if (has_lz4) {
                            if (!scan_compressed_lz4(columns, decode_needed,
                                                     segment.RowCount())) {
                                return false;
                            }
                        } else {
                            if (!build_readable_columns(columns, decode_needed, &decoded_cache,
                                                        &readable_cache)) {
                                return false;
                            }
                            if (!scan_compressed(readable_cache, segment.RowCount())) {
                                return false;
                            }
                        }
                    } else {
                        if (!scan_fields(segment.Fields(), segment.RowCount())) {
                            return false;
                        }
                    }
                    return true;
                })) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }

        uint32_t row_count = outputs.empty() ? 0u
                                             : static_cast<uint32_t>(outputs.front().validity.size());
        std::vector<uint8_t> out;
        out.resize(sizeof(uint32_t) + sizeof(uint16_t));
        size_t out_cursor = 0;
        std::memcpy(out.data() + out_cursor, &row_count, sizeof(uint32_t));
        out_cursor += sizeof(uint32_t);
        const uint16_t out_field_count = static_cast<uint16_t>(outputs.size());
        std::memcpy(out.data() + out_cursor, &out_field_count, sizeof(uint16_t));
        out_cursor += sizeof(uint16_t);

        auto pack_validity = [](const std::vector<uint8_t>& flags) {
            std::vector<uint8_t> bits((flags.size() + 7) / 8, 0);
            for (size_t i = 0; i < flags.size(); ++i) {
                if (flags[i]) {
                    bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
                }
            }
            return bits;
        };

        for (auto& col : outputs) {
            const uint8_t type = EncodeFieldType(col.type);
            const uint8_t validity_mode = col.has_null ? 1 : 0;
            const uint32_t count = row_count;
            const size_t header_size = sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint8_t) +
                                       sizeof(uint32_t);
            const size_t data_bytes = col.data.size();
            std::vector<uint8_t> validity_bits;
            if (validity_mode == 1) {
                validity_bits = pack_validity(col.validity);
            }
            const size_t validity_bytes = validity_bits.size();
            const bool varlen = col.type == FieldType::kString || col.type == FieldType::kBytes;
            const size_t lengths_bytes = varlen ? (col.lengths.size() * sizeof(uint32_t)) : 0;
            const size_t extra_header = varlen ? sizeof(uint32_t) : 0;
            const size_t needed = header_size + extra_header + lengths_bytes + data_bytes +
                                  validity_bytes;
            const size_t start = out.size();
            out.resize(start + needed);
            size_t cursor_local = start;
            std::memcpy(out.data() + cursor_local, &col.index, sizeof(uint16_t));
            cursor_local += sizeof(uint16_t);
            std::memcpy(out.data() + cursor_local, &type, sizeof(uint8_t));
            cursor_local += sizeof(uint8_t);
            std::memcpy(out.data() + cursor_local, &validity_mode, sizeof(uint8_t));
            cursor_local += sizeof(uint8_t);
            std::memcpy(out.data() + cursor_local, &count, sizeof(uint32_t));
            cursor_local += sizeof(uint32_t);
            if (varlen) {
                const uint32_t bytes_size = static_cast<uint32_t>(data_bytes);
                std::memcpy(out.data() + cursor_local, &bytes_size, sizeof(uint32_t));
                cursor_local += sizeof(uint32_t);
                if (!col.lengths.empty()) {
                    std::memcpy(out.data() + cursor_local, col.lengths.data(), lengths_bytes);
                }
                cursor_local += lengths_bytes;
            }
            if (!col.data.empty()) {
                std::memcpy(out.data() + cursor_local, col.data.data(), col.data.size());
            }
            cursor_local += col.data.size();
            if (validity_mode == 1 && !validity_bits.empty()) {
                std::memcpy(out.data() + cursor_local, validity_bits.data(), validity_bits.size());
            }
        }

        if (compression_metrics_) {
            const double decode_sec = scan_metrics.decode_ns > 0
                ? static_cast<double>(scan_metrics.decode_ns) / 1e9
                : 0.0;
            const double pred_sec = scan_metrics.predicate_ns > 0
                ? static_cast<double>(scan_metrics.predicate_ns) / 1e9
                : 0.0;
            const double out_sec = scan_metrics.output_ns > 0
                ? static_cast<double>(scan_metrics.output_ns) / 1e9
                : 0.0;
            const double decode_bps = decode_sec > 0.0
                ? static_cast<double>(scan_metrics.decode_bytes) / decode_sec
                : 0.0;
            const double raw_bps = decode_sec > 0.0
                ? static_cast<double>(scan_metrics.raw_bytes) / decode_sec
                : 0.0;
            std::cerr << "compression_scan_metrics"
                      << " raw_bytes=" << scan_metrics.raw_bytes
                      << " decode_bytes=" << scan_metrics.decode_bytes
                      << " raw_bytes_per_sec=" << static_cast<uint64_t>(raw_bps)
                      << " decode_bytes_per_sec=" << static_cast<uint64_t>(decode_bps)
                      << " compressed_columns_touched="
                      << scan_metrics.compressed_columns_touched
                      << " compressed_columns_skipped="
                      << scan_metrics.compressed_columns_skipped
                      << " decode_ms=" << static_cast<uint64_t>(decode_sec * 1000.0)
                      << " predicate_ms=" << static_cast<uint64_t>(pred_sec * 1000.0)
                      << " output_ms=" << static_cast<uint64_t>(out_sec * 1000.0)
                      << "\n";
        }

        SendStatus(client, header, Status::kOk, out);
    }

    void HandleHealth(int client, const MessageHeader& header) {
        uint16_t dataset_count = 0;
        uint64_t segment_count = 0;
        uint64_t row_count = 0;
        for (const auto& db_entry : databases_) {
            if (IsAuthDatabaseName(db_entry.first)) {
                continue;
            }
            for (const auto& entry : db_entry.second.datasets) {
                const auto& dataset = *entry.second.dataset;
                segment_count += dataset.Segments().size();
                row_count += dataset.RowCount();
                if (dataset_count < UINT16_MAX) {
                    dataset_count += 1;
                }
            }
        }
        std::vector<uint8_t> out(sizeof(uint16_t) + sizeof(uint64_t) * 2);
        size_t cursor = 0;
        std::memcpy(out.data() + cursor, &dataset_count, sizeof(uint16_t));
        cursor += sizeof(uint16_t);
        std::memcpy(out.data() + cursor, &segment_count, sizeof(uint64_t));
        cursor += sizeof(uint64_t);
        std::memcpy(out.data() + cursor, &row_count, sizeof(uint64_t));
        SendStatus(client, header, Status::kOk, out);
    }

    void HandleHostKey(int client, const MessageHeader& header) {
        std::vector<uint8_t> out;
        const uint16_t key_len = static_cast<uint16_t>(host_key_pub_.size());
        const uint16_t fp_len = static_cast<uint16_t>(host_key_fingerprint_.size());
        out.reserve(sizeof(uint16_t) + key_len + sizeof(uint16_t) + fp_len);
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&key_len),
                   reinterpret_cast<const uint8_t*>(&key_len) + sizeof(key_len));
        out.insert(out.end(), host_key_pub_.begin(), host_key_pub_.end());
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&fp_len),
                   reinterpret_cast<const uint8_t*>(&fp_len) + sizeof(fp_len));
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(host_key_fingerprint_.data()),
                   reinterpret_cast<const uint8_t*>(host_key_fingerprint_.data()) + fp_len);
        SendStatus(client, header, Status::kOk, out);
    }

    void HandleHostKeyRotate(int client, const MessageHeader& header) {
        const std::string remote = GetRemoteAddr(client);
        if (!IsLocalRemote(remote)) {
            SendStatus(client, header, Status::kPermissionDenied, {});
            return;
        }
        std::array<uint8_t, kEd25519PrivBytes> new_priv{};
        std::array<uint8_t, kHostKeyBytes> new_pub{};
        std::string new_fingerprint;
        if (!EnsureHostKey(host_key_path_, true, &new_priv, &new_pub, &new_fingerprint)) {
            SendStatus(client, header, Status::kInternalError, {});
            return;
        }
        host_key_priv_ = new_priv;
        host_key_pub_.assign(new_pub.begin(), new_pub.end());
        host_key_fingerprint_ = new_fingerprint;
        AppendAuditLog("server.key.rotate", remote, "", "{}");
        std::vector<uint8_t> out;
        const uint16_t key_len = static_cast<uint16_t>(host_key_pub_.size());
        const uint16_t fp_len = static_cast<uint16_t>(host_key_fingerprint_.size());
        out.reserve(2 + key_len + 2 + fp_len);
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&key_len),
                   reinterpret_cast<const uint8_t*>(&key_len) + sizeof(key_len));
        out.insert(out.end(), host_key_pub_.begin(), host_key_pub_.end());
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&fp_len),
                   reinterpret_cast<const uint8_t*>(&fp_len) + sizeof(fp_len));
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(host_key_fingerprint_.data()),
                   reinterpret_cast<const uint8_t*>(host_key_fingerprint_.data()) + fp_len);
        SendStatus(client, header, Status::kOk, out);
    }

    void HandleAuthInitRoot(int client, const MessageHeader& header,
                            const std::vector<uint8_t>& payload) {
        const std::string remote = GetRemoteAddr(client);
        if (!IsLocalRemote(remote)) {
            SendStatus(client, header, Status::kPermissionDenied, {});
            return;
        }
        if (IsRootInitialized(auth_db_path_)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::string pubkey;
        std::string comment;
        std::string claimed_fingerprint;
        if (!ParseKeyRegistrationPayload(payload, &pubkey, &comment,
                                         &claimed_fingerprint)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        const std::string fingerprint =
            Sha256Hex(reinterpret_cast<const uint8_t*>(pubkey.data()), pubkey.size());
        if (!claimed_fingerprint.empty() && claimed_fingerprint != fingerprint) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        AppendKeyRecord(fingerprint, pubkey, comment, true);
        AppendKeyRole(fingerprint, "root", "*");
        std::filesystem::create_directories(auth_db_path_);
        const std::filesystem::path root_marker =
            std::filesystem::path(auth_db_path_) / "root_initialized";
        std::ofstream marker(root_marker, std::ios::binary | std::ios::trunc);
        marker << "ok\n";
        AppendAuditLog("root.init", remote, fingerprint, "{\"note\":\"init_root\"}");
        if (!FlushDatabaseActiveSegments(kAuthDbName)) {
            SendStatus(client, header, Status::kInternalError, {});
            return;
        }
        std::vector<uint8_t> out;
        const uint16_t fp_len = static_cast<uint16_t>(fingerprint.size());
        out.reserve(sizeof(uint16_t) + fp_len);
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&fp_len),
                   reinterpret_cast<const uint8_t*>(&fp_len) + sizeof(fp_len));
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(fingerprint.data()),
                   reinterpret_cast<const uint8_t*>(fingerprint.data()) + fp_len);
        SendStatus(client, header, Status::kOk, out);
    }

    void HandleAuthKeyAdd(int client, const MessageHeader& header,
                          const std::vector<uint8_t>& payload) {
        std::string pubkey;
        std::string comment;
        std::string claimed_fingerprint;
        if (!ParseKeyRegistrationPayload(payload, &pubkey, &comment,
                                         &claimed_fingerprint)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        const std::string fingerprint =
            Sha256Hex(reinterpret_cast<const uint8_t*>(pubkey.data()), pubkey.size());
        if (!claimed_fingerprint.empty() && claimed_fingerprint != fingerprint) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        AppendKeyRecord(fingerprint, pubkey, comment, true);
        AppendAuditLog("auth.key.add", GetRemoteAddr(client), fingerprint,
                       "{\"comment\":\"" + comment + "\"}");
        std::vector<uint8_t> out;
        const uint16_t fp_len = static_cast<uint16_t>(fingerprint.size());
        out.reserve(sizeof(uint16_t) + fp_len);
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&fp_len),
                   reinterpret_cast<const uint8_t*>(&fp_len) + sizeof(fp_len));
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(fingerprint.data()),
                   reinterpret_cast<const uint8_t*>(fingerprint.data()) + fp_len);
        SendStatus(client, header, Status::kOk, out);
    }

    void HandleAuthKeyDisable(int client, const MessageHeader& header,
                              const std::vector<uint8_t>& payload,
                              bool remove_key) {
        size_t cursor = 0;
        std::string fingerprint;
        if (!ReadName(payload, &cursor, &fingerprint)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        AppendKeyRecord(fingerprint, "", remove_key ? "removed" : "disabled", false);
        AppendAuditLog(remove_key ? "auth.key.remove" : "auth.key.disable",
                       GetRemoteAddr(client), fingerprint, "{}");
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleAuthKeyList(int client, const MessageHeader& header) {
        struct KeyInfo {
            std::string comment;
            bool enabled = true;
        };
        std::unordered_map<std::string, KeyInfo> keys;
        auto db_it = databases_.find(kAuthDbName);
        if (db_it != databases_.end()) {
            auto ds_it = db_it->second.datasets.find("keys");
            if (ds_it != db_it->second.datasets.end()) {
                const auto& schema_fields = ds_it->second.dataset->Fields();
                const auto resolve_index = [](const std::vector<FieldVector>& fields,
                                              const std::string& name,
                                              size_t fallback) -> size_t {
                    for (size_t i = 0; i < fields.size(); ++i) {
                        if (fields[i].Name() == name) {
                            return i;
                        }
                    }
                    return fallback < fields.size() ? fallback : fields.size();
                };
                const size_t fp_idx = resolve_index(schema_fields, "fingerprint", 1);
                const size_t comment_idx = resolve_index(schema_fields, "comment", 3);
                const size_t enabled_idx = resolve_index(schema_fields, "enabled", 4);
                const auto scan_dataset = [&](const DatasetState& state,
                                              const std::function<void(
                                                  const std::vector<FieldVector>&, size_t)>& fn) {
                    for (const auto& segment : state.dataset->Segments()) {
                        const auto& fields = segment.Fields();
                        for (size_t row = 0; row < segment.RowCount(); ++row) {
                            fn(fields, row);
                        }
                    }
                    const auto& active = state.dataset->ActiveFields();
                    if (!active.empty()) {
                        const size_t rows = state.dataset->ActiveRowCount();
                        for (size_t row = 0; row < rows; ++row) {
                            fn(active, row);
                        }
                    }
                };
                scan_dataset(ds_it->second, [&](const std::vector<FieldVector>& fields,
                                                size_t row) {
                    std::string fingerprint;
                    if (!ReadStringField(fields[fp_idx], row, &fingerprint)) {
                        return;
                    }
                    std::string comment;
                    bool enabled = true;
                    ReadStringField(fields[comment_idx], row, &comment);
                    ReadBoolField(fields[enabled_idx], row, &enabled);
                    keys[fingerprint] = {comment, enabled};
                });
            }
        }
        std::vector<uint8_t> out;
        const uint16_t count = static_cast<uint16_t>(
            std::min<size_t>(keys.size(), UINT16_MAX));
        out.resize(sizeof(uint16_t));
        std::memcpy(out.data(), &count, sizeof(uint16_t));
        size_t written = 0;
        for (const auto& entry : keys) {
            if (written >= count) {
                break;
            }
            const auto& fingerprint = entry.first;
            const auto& info = entry.second;
            const uint16_t fp_len = static_cast<uint16_t>(fingerprint.size());
            const uint16_t comment_len = static_cast<uint16_t>(info.comment.size());
            const size_t offset = out.size();
            out.resize(offset + sizeof(uint16_t) + fp_len + 1 + sizeof(uint16_t) + comment_len);
            std::memcpy(out.data() + offset, &fp_len, sizeof(uint16_t));
            std::memcpy(out.data() + offset + sizeof(uint16_t),
                        fingerprint.data(), fp_len);
            out[offset + sizeof(uint16_t) + fp_len] = info.enabled ? 1 : 0;
            std::memcpy(out.data() + offset + sizeof(uint16_t) + fp_len + 1,
                        &comment_len, sizeof(uint16_t));
            std::memcpy(out.data() + offset + sizeof(uint16_t) + fp_len + 1 +
                            sizeof(uint16_t),
                        info.comment.data(), comment_len);
            written += 1;
        }
        SendStatus(client, header, Status::kOk, out);
    }

    void HandleAuthRoleCreate(int client, const MessageHeader& header,
                              const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string role_name;
        if (!ReadName(payload, &cursor, &role_name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        AppendRoleRecord(role_name, false);
        AppendAuditLog("auth.role.create", GetRemoteAddr(client), "",
                       "{\"role\":\"" + role_name + "\"}");
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleAuthRoleDelete(int client, const MessageHeader& header,
                              const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string role_name;
        if (!ReadName(payload, &cursor, &role_name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        AuthSnapshot snapshot;
        LoadAuthSnapshot(&snapshot);
        auto grants_it = snapshot.role_grants.find(role_name);
        if (grants_it != snapshot.role_grants.end()) {
            for (const auto& grant : grants_it->second) {
                AppendRoleGrant(role_name, "!" + grant.first, grant.second);
            }
        }
        for (const auto& entry : snapshot.key_roles) {
            const auto& fp = entry.first;
            for (const auto& role_entry : entry.second) {
                if (role_entry.first == role_name) {
                    AppendKeyRole(fp, "!" + role_name, role_entry.second);
                }
            }
        }
        AppendAuditLog("auth.role.delete", GetRemoteAddr(client), "",
                       "{\"role\":\"" + role_name + "\"}");
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleAuthRoleGrant(int client, const MessageHeader& header,
                             const std::vector<uint8_t>& payload,
                             bool revoke) {
        size_t cursor = 0;
        std::string role_name;
        std::string cap;
        std::string scope;
        if (!ReadName(payload, &cursor, &role_name) ||
            !ReadName(payload, &cursor, &cap) ||
            !ReadName(payload, &cursor, &scope)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        const std::string stored = revoke ? "!" + cap : cap;
        AppendRoleGrant(role_name, stored, scope);
        AppendAuditLog(revoke ? "auth.role.revoke" : "auth.role.grant",
                       GetRemoteAddr(client), "",
                       "{\"role\":\"" + role_name + "\",\"cap\":\"" + cap +
                       "\",\"scope\":\"" + scope + "\"}");
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleAuthAssignRole(int client, const MessageHeader& header,
                              const std::vector<uint8_t>& payload,
                              bool revoke) {
        size_t cursor = 0;
        std::string fingerprint;
        std::string role_name;
        std::string scope;
        if (!ReadName(payload, &cursor, &fingerprint) ||
            !ReadName(payload, &cursor, &role_name) ||
            !ReadName(payload, &cursor, &scope)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        const std::string stored = revoke ? "!" + role_name : role_name;
        AppendKeyRole(fingerprint, stored, scope);
        AppendAuditLog(revoke ? "auth.role.unassign" : "auth.role.assign",
                       GetRemoteAddr(client), fingerprint,
                       "{\"role\":\"" + role_name + "\",\"scope\":\"" + scope + "\"}");
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleAuthGrantKey(int client, const MessageHeader& header,
                            const std::vector<uint8_t>& payload,
                            bool revoke) {
        size_t cursor = 0;
        std::string fingerprint;
        std::string cap;
        std::string scope;
        if (!ReadName(payload, &cursor, &fingerprint) ||
            !ReadName(payload, &cursor, &cap) ||
            !ReadName(payload, &cursor, &scope)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        const std::string stored = revoke ? "!" + cap : cap;
        AppendKeyGrant(fingerprint, stored, scope);
        AppendAuditLog(revoke ? "auth.key.revoke" : "auth.key.grant",
                       GetRemoteAddr(client), fingerprint,
                       "{\"cap\":\"" + cap + "\",\"scope\":\"" + scope + "\"}");
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleAuthRateLimitList(int client, const MessageHeader& header) {
        std::vector<uint8_t> out;
        const uint16_t count = static_cast<uint16_t>(
            std::min<size_t>(rate_limit_state_.size(), UINT16_MAX));
        out.resize(sizeof(uint16_t));
        std::memcpy(out.data(), &count, sizeof(uint16_t));
        size_t written = 0;
        for (const auto& entry : rate_limit_state_) {
            if (written >= count) {
                break;
            }
            const auto& key = entry.first;
            const auto split = key.find('|');
            const std::string remote = key.substr(0, split);
            const std::string fingerprint =
                split == std::string::npos ? "" : key.substr(split + 1);
            const uint16_t remote_len = static_cast<uint16_t>(remote.size());
            const uint16_t fp_len = static_cast<uint16_t>(fingerprint.size());
            const int64_t next_epoch = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    entry.second.next_allowed.time_since_epoch()).count());
            const int64_t last_epoch = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    entry.second.last_fail.time_since_epoch()).count());
            const size_t offset = out.size();
            out.resize(offset + sizeof(uint16_t) + remote_len +
                       sizeof(uint16_t) + fp_len +
                       sizeof(uint32_t) + sizeof(int64_t) + sizeof(int64_t));
            std::memcpy(out.data() + offset, &remote_len, sizeof(uint16_t));
            std::memcpy(out.data() + offset + sizeof(uint16_t),
                        remote.data(), remote_len);
            std::memcpy(out.data() + offset + sizeof(uint16_t) + remote_len,
                        &fp_len, sizeof(uint16_t));
            std::memcpy(out.data() + offset + sizeof(uint16_t) + remote_len +
                            sizeof(uint16_t),
                        fingerprint.data(), fp_len);
            const size_t tail = offset + sizeof(uint16_t) + remote_len +
                                sizeof(uint16_t) + fp_len;
            const uint32_t fail = entry.second.fail_count;
            std::memcpy(out.data() + tail, &fail, sizeof(uint32_t));
            std::memcpy(out.data() + tail + sizeof(uint32_t),
                        &next_epoch, sizeof(int64_t));
            std::memcpy(out.data() + tail + sizeof(uint32_t) + sizeof(int64_t),
                        &last_epoch, sizeof(int64_t));
            written += 1;
        }
        SendStatus(client, header, Status::kOk, out);
    }

    void HandleAuthRateLimitClear(int client, const MessageHeader& header,
                                  const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string remote;
        std::string fingerprint;
        if (!ReadName(payload, &cursor, &remote)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        if (cursor < payload.size()) {
            ReadName(payload, &cursor, &fingerprint);
        }
        std::vector<std::string> keys_to_clear;
        for (const auto& entry : rate_limit_state_) {
            const auto split = entry.first.find('|');
            const std::string entry_remote = entry.first.substr(0, split);
            const std::string entry_fp =
                split == std::string::npos ? "" : entry.first.substr(split + 1);
            if (entry_remote != remote) {
                continue;
            }
            if (!fingerprint.empty() && entry_fp != fingerprint) {
                continue;
            }
            keys_to_clear.push_back(entry.first);
        }
        for (const auto& key : keys_to_clear) {
            const auto split = key.find('|');
            const std::string entry_remote = key.substr(0, split);
            const std::string entry_fp =
                split == std::string::npos ? "" : key.substr(split + 1);
            rate_limit_state_.erase(key);
            RateLimitState cleared{};
            AppendRateLimitRow(entry_remote, entry_fp, cleared);
        }
        AppendAuditLog("auth.ratelimit.clear", GetRemoteAddr(client), "",
                       "{\"remote\":\"" + remote + "\"}");
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleAuthWhoami(int client, const MessageHeader& header) {
        SessionInfo session;
        if (!GetSessionForClient(client, &session)) {
            SendStatus(client, header, Status::kAuthFailed, {});
            return;
        }
        AuthSnapshot snapshot;
        if (!LoadAuthSnapshot(&snapshot)) {
            SendStatus(client, header, Status::kInternalError, {});
            return;
        }
        auto roles = snapshot.key_roles[session.fingerprint];
        const auto& role_revokes = snapshot.key_role_revokes[session.fingerprint];
        std::vector<std::pair<std::string, std::string>> filtered_roles;
        for (const auto& entry : roles) {
            bool revoked = false;
            for (const auto& revoke : role_revokes) {
                if (revoke.first != entry.first) {
                    continue;
                }
                if (revoke.second == "*" || revoke.second == entry.second) {
                    revoked = true;
                    break;
                }
            }
            if (!revoked) {
                filtered_roles.push_back(entry);
            }
        }
        auto grants = snapshot.key_grants[session.fingerprint];
        const auto& grant_revokes = snapshot.key_grant_revokes[session.fingerprint];
        std::vector<std::pair<std::string, std::string>> filtered_grants;
        for (const auto& entry : grants) {
            bool revoked = false;
            for (const auto& revoke : grant_revokes) {
                if (revoke.first != entry.first) {
                    continue;
                }
                if (revoke.second == "*" || revoke.second == entry.second) {
                    revoked = true;
                    break;
                }
            }
            if (!revoked) {
                filtered_grants.push_back(entry);
            }
        }
        std::vector<uint8_t> out;
        const uint16_t fp_len = static_cast<uint16_t>(session.fingerprint.size());
        const uint16_t role_count = static_cast<uint16_t>(
            std::min<size_t>(filtered_roles.size(), UINT16_MAX));
        const uint16_t grant_count = static_cast<uint16_t>(
            std::min<size_t>(filtered_grants.size(), UINT16_MAX));
        out.reserve(sizeof(uint16_t) + fp_len +
                    sizeof(uint16_t) + sizeof(uint16_t));
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&fp_len),
                   reinterpret_cast<const uint8_t*>(&fp_len) + sizeof(fp_len));
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(session.fingerprint.data()),
                   reinterpret_cast<const uint8_t*>(session.fingerprint.data()) + fp_len);
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&role_count),
                   reinterpret_cast<const uint8_t*>(&role_count) + sizeof(role_count));
        size_t role_written = 0;
        for (const auto& entry : filtered_roles) {
            if (role_written >= role_count) {
                break;
            }
            const uint16_t name_len = static_cast<uint16_t>(entry.first.size());
            const uint16_t scope_len = static_cast<uint16_t>(entry.second.size());
            out.insert(out.end(),
                       reinterpret_cast<const uint8_t*>(&name_len),
                       reinterpret_cast<const uint8_t*>(&name_len) + sizeof(name_len));
            out.insert(out.end(),
                       reinterpret_cast<const uint8_t*>(entry.first.data()),
                       reinterpret_cast<const uint8_t*>(entry.first.data()) + name_len);
            out.insert(out.end(),
                       reinterpret_cast<const uint8_t*>(&scope_len),
                       reinterpret_cast<const uint8_t*>(&scope_len) + sizeof(scope_len));
            out.insert(out.end(),
                       reinterpret_cast<const uint8_t*>(entry.second.data()),
                       reinterpret_cast<const uint8_t*>(entry.second.data()) + scope_len);
            role_written += 1;
        }
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&grant_count),
                   reinterpret_cast<const uint8_t*>(&grant_count) + sizeof(grant_count));
        size_t grant_written = 0;
        for (const auto& entry : filtered_grants) {
            if (grant_written >= grant_count) {
                break;
            }
            const uint16_t cap_len = static_cast<uint16_t>(entry.first.size());
            const uint16_t scope_len = static_cast<uint16_t>(entry.second.size());
            out.insert(out.end(),
                       reinterpret_cast<const uint8_t*>(&cap_len),
                       reinterpret_cast<const uint8_t*>(&cap_len) + sizeof(cap_len));
            out.insert(out.end(),
                       reinterpret_cast<const uint8_t*>(entry.first.data()),
                       reinterpret_cast<const uint8_t*>(entry.first.data()) + cap_len);
            out.insert(out.end(),
                       reinterpret_cast<const uint8_t*>(&scope_len),
                       reinterpret_cast<const uint8_t*>(&scope_len) + sizeof(scope_len));
            out.insert(out.end(),
                       reinterpret_cast<const uint8_t*>(entry.second.data()),
                       reinterpret_cast<const uint8_t*>(entry.second.data()) + scope_len);
            grant_written += 1;
        }
        SendStatus(client, header, Status::kOk, out);
    }

    void HandleCreateDatabase(int client, const MessageHeader& header,
                              const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string name;
        if (!ReadName(payload, &cursor, &name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        if (IsAuthDatabaseName(name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        EnsureDatabase(name);
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleListDatabases(int client, const MessageHeader& header) {
        std::vector<uint8_t> out;
        std::vector<std::string> names;
        names.reserve(databases_.size());
        for (const auto& entry : databases_) {
            if (IsAuthDatabaseName(entry.first)) {
                continue;
            }
            names.push_back(entry.first);
        }
        const uint16_t count = static_cast<uint16_t>(
            std::min<size_t>(names.size(), UINT16_MAX));
        out.resize(sizeof(uint16_t));
        std::memcpy(out.data(), &count, sizeof(uint16_t));
        size_t written = 0;
        for (const auto& name : names) {
            if (written >= count) {
                break;
            }
            const uint16_t len = static_cast<uint16_t>(name.size());
            const size_t offset = out.size();
            out.resize(offset + sizeof(uint16_t) + name.size());
            std::memcpy(out.data() + offset, &len, sizeof(uint16_t));
            std::memcpy(out.data() + offset + sizeof(uint16_t), name.data(), name.size());
            written += 1;
        }
        SendStatus(client, header, Status::kOk, out);
    }

    void SendStatus(int client, const MessageHeader& request, Status status,
                    const std::vector<uint8_t>& payload) {
        MessageHeader response{};
        response.flags = 1;
        response.opcode = request.opcode;
        response.status = static_cast<uint16_t>(status);
        response.payload_size = static_cast<uint32_t>(payload.size());
        response.request_id = request.request_id;
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            auto it = connection_states_.find(client);
            if (it != connection_states_.end()) {
                auto& channel = it->second.channel;
                std::vector<uint8_t> plain(sizeof(response) + payload.size());
                std::memcpy(plain.data(), &response, sizeof(response));
                if (!payload.empty()) {
                    std::memcpy(plain.data() + sizeof(response), payload.data(),
                                payload.size());
                }
                WriteSecureFrame(client, channel.key_s2c, channel.send_seq,
                                 plain.data(), plain.size());
                channel.send_seq += 1;
                return;
            }
        }
        WriteExact(client, &response, sizeof(response));
        if (!payload.empty()) {
            WriteStream(client, payload.data(), payload.size(), 64 * 1024);
        }
    }

    void SendPermissionDenied(int client,
                              const MessageHeader& request,
                              const std::string& capability,
                              const std::string& scope) {
        std::vector<uint8_t> payload;
        const uint16_t cap_len = static_cast<uint16_t>(capability.size());
        const uint16_t scope_len = static_cast<uint16_t>(scope.size());
        payload.reserve(sizeof(uint16_t) + cap_len + sizeof(uint16_t) + scope_len);
        payload.insert(payload.end(),
                       reinterpret_cast<const uint8_t*>(&cap_len),
                       reinterpret_cast<const uint8_t*>(&cap_len) + sizeof(cap_len));
        payload.insert(payload.end(),
                       reinterpret_cast<const uint8_t*>(capability.data()),
                       reinterpret_cast<const uint8_t*>(capability.data()) + cap_len);
        payload.insert(payload.end(),
                       reinterpret_cast<const uint8_t*>(&scope_len),
                       reinterpret_cast<const uint8_t*>(&scope_len) + sizeof(scope_len));
        payload.insert(payload.end(),
                       reinterpret_cast<const uint8_t*>(scope.data()),
                       reinterpret_cast<const uint8_t*>(scope.data()) + scope_len);
        SendStatus(client, request, Status::kPermissionDenied, payload);
    }

    void SendRateLimited(int client, const MessageHeader& request, uint32_t wait_seconds) {
        std::vector<uint8_t> payload(sizeof(uint32_t));
        std::memcpy(payload.data(), &wait_seconds, sizeof(wait_seconds));
        SendStatus(client, request, Status::kRateLimited, payload);
    }

    std::string bind_addr_;
    uint16_t port_ = 0;
    std::string storage_root_;
    std::string auth_db_path_;
    std::string host_key_path_;
    std::array<uint8_t, kEd25519PrivBytes> host_key_priv_{};
    std::vector<uint8_t> host_key_pub_;
    std::string host_key_fingerprint_;
    uint32_t session_idle_timeout_sec_ = 0;
    uint32_t session_max_lifetime_sec_ = 0;
    size_t session_max_total_ = 0;
    size_t session_max_per_fingerprint_ = 0;
    uint32_t handshake_nonce_ttl_sec_ = 0;
    size_t handshake_nonce_max_entries_ = 0;
    int auth_failure_delay_ms_ = 0;
    uint32_t auth_rate_limit_burst_ = 0;
    std::vector<uint32_t> auth_rate_limit_backoff_sec_;
    size_t auth_rate_limit_state_max_entries_ = 0;
    uint32_t auth_rate_limit_state_ttl_sec_ = 0;
    size_t auth_rate_limit_max_rows_ = 0;
    size_t auth_audit_log_max_rows_ = 0;
    size_t auth_prune_batch_rows_ = 0;
    bool flush_on_shutdown_ = false;
    bool flush_on_seal_ = true;
    int flush_interval_ms_ = 0;
    uint32_t max_payload_bytes_ = 0;
    uint32_t max_rows_per_batch_ = 0;
    int append_sleep_ms_ = 0;
    size_t segment_cache_max_ = 0;
    uint64_t segment_cache_bytes_ = 0;
    size_t query_threads_ = 1;
    bool compression_metrics_ = false;
    std::unordered_map<std::string, DatabaseState> databases_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> active_clients_{0};
    std::atomic<bool> housekeeping_running_{false};
    std::thread housekeeping_thread_;
    static Server* instance_;
    std::chrono::steady_clock::time_point last_active_flush_{};
    std::mutex connection_mutex_;
    std::unordered_map<int, ConnectionState> connection_states_{};
    std::mutex session_mutex_;
    std::unordered_map<std::string, SessionInfo> session_store_{};
    std::unordered_map<std::string, RateLimitState> rate_limit_state_{};
    bool audit_log_prune_pending_ = false;
    bool rate_limit_prune_pending_ = false;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        handshake_nonce_cache_{};
    std::atomic<uint64_t> authz_denied_counter_{0};

    std::string DatabasePath(const std::string& name) const {
        if (IsAuthDatabaseName(name) && !auth_db_path_.empty()) {
            return auth_db_path_;
        }
        return (std::filesystem::path(storage_root_) / name).string();
    }

    std::string DatasetPath(const std::string& db_name, const std::string& dataset_name) const {
        return (std::filesystem::path(DatabasePath(db_name)) / dataset_name).string();
    }

    std::string SchemaPath(const std::string& db_name, const std::string& dataset_name) const {
        return (std::filesystem::path(DatasetPath(db_name, dataset_name)) / "schema.bin")
            .string();
    }

    std::string SegmentPath(const std::string& db_name, const std::string& dataset_name,
                            size_t index) const {
        return (std::filesystem::path(DatasetPath(db_name, dataset_name)) /
                ("segment_" + std::to_string(index) + ".mimicdb"))
            .string();
    }

    uint64_t SegmentBytes(const Segment& segment) const {
        uint64_t bytes = 0;
        for (const auto& field : segment.Fields()) {
            switch (field.Type()) {
                case FieldType::kInt32:
                    bytes += static_cast<uint64_t>(field.Size() * sizeof(int32_t));
                    break;
                case FieldType::kInt64:
                    bytes += static_cast<uint64_t>(field.Size() * sizeof(int64_t));
                    break;
                case FieldType::kFloat64:
                    bytes += static_cast<uint64_t>(field.Size() * sizeof(double));
                    break;
                case FieldType::kBool:
                    bytes += static_cast<uint64_t>(field.Size() * sizeof(uint8_t));
                    break;
                case FieldType::kDictInt32:
                    bytes += static_cast<uint64_t>(field.Size() * sizeof(uint32_t));
                    break;
                case FieldType::kString:
                case FieldType::kBytes:
                case FieldType::kArray:
                    bytes += static_cast<uint64_t>(field.Size() * sizeof(uint32_t));
                    bytes += static_cast<uint64_t>(field.BytesSize());
                    break;
                case FieldType::kObject:
                    break;
            }
            if (field.HasNulls()) {
                bytes += static_cast<uint64_t>(field.Validity().WordCount() * sizeof(uint64_t));
            }
        }
        return bytes;
    }

    void DropCachedSegments(DatasetState& state, size_t drop_count) {
        if (drop_count == 0) {
            return;
        }
        const auto& segments = state.dataset->Segments();
        const size_t capped = drop_count > segments.size() ? segments.size() : drop_count;
        uint64_t reclaimed = 0;
        for (size_t i = 0; i < capped; ++i) {
            reclaimed += SegmentBytes(segments[i]);
        }
        state.dataset->DropSegments(capped);
        state.segment_base_index += capped;
        if (reclaimed > state.cached_bytes) {
            state.cached_bytes = 0;
        } else {
            state.cached_bytes -= reclaimed;
        }
    }

    void EnforceSegmentCacheLimits(DatasetState& state) {
        if (segment_cache_max_ > 0 &&
            state.dataset->Segments().size() > segment_cache_max_) {
            const size_t drop = state.dataset->Segments().size() - segment_cache_max_;
            DropCachedSegments(state, drop);
        }
        if (segment_cache_bytes_ > 0 && state.cached_bytes > segment_cache_bytes_) {
            while (!state.dataset->Segments().empty() &&
                   state.cached_bytes > segment_cache_bytes_) {
                DropCachedSegments(state, 1);
            }
        }
    }

    bool ForEachSegment(const std::string& db_name, DatasetState& state,
                        const std::function<bool(const Segment&)>& visitor) {
        const size_t in_memory_base = state.segment_base_index;
        const size_t in_memory_count = state.dataset->Segments().size();
        const size_t in_memory_end = in_memory_base + in_memory_count;
        for (size_t idx = 0; idx < state.persisted_segments; ++idx) {
            if (idx >= in_memory_base && idx < in_memory_end) {
                const Segment& seg = state.dataset->Segments()[idx - in_memory_base];
                if (!visitor(seg)) {
                    return false;
                }
                continue;
            }
            Segment segment(0, {});
            SegmentReader reader(SegmentPath(db_name, state.dataset->Name(), idx));
            if (!reader.Read(&segment)) {
                return false;
            }
            if (!visitor(segment)) {
                return false;
            }
        }
        const auto& active = state.dataset->ActiveFields();
        if (!active.empty()) {
            Segment active_segment(state.dataset->SegmentCapacity(),
                                   state.dataset->ActiveRowCount(), active);
            if (!visitor(active_segment)) {
                return false;
            }
        }
        return true;
    }

    DatabaseState& EnsureDatabase(const std::string& name) {
        auto [it, inserted] = databases_.emplace(name, DatabaseState{});
        if (inserted) {
            it->second.name = name;
            it->second.path = DatabasePath(name);
            if (!std::filesystem::exists(it->second.path)) {
                std::filesystem::create_directories(it->second.path);
            }
        }
        return it->second;
    }

    bool CreateDatasetInternal(DatabaseState& db_state,
                               const std::string& name,
                               const std::vector<std::pair<std::string, FieldType>>& fields) {
        if (db_state.datasets.find(name) != db_state.datasets.end()) {
            return true;
        }
        auto dataset = std::make_unique<Dataset>(name);
        for (const auto& field : fields) {
            dataset->AddField(FieldVector(field.first, field.second));
        }
        DatasetState state;
        state.dataset = std::move(dataset);
        state.path = DatasetPath(db_state.name, name);
        if (!std::filesystem::exists(state.path)) {
            std::filesystem::create_directories(state.path);
        }
        if (!WriteSchema(state.path, *state.dataset)) {
            return false;
        }
        db_state.datasets[name] = std::move(state);
        return true;
    }

    bool ReadStringField(const FieldVector& field, size_t row, std::string* out) const {
        if (row >= field.Size()) {
            return false;
        }
        if (field.HasNulls() && !field.IsValid(row)) {
            return false;
        }
        if (field.Type() == FieldType::kString || field.Type() == FieldType::kBytes) {
            const uint32_t* offsets = field.DataOffsets();
            const uint32_t* lengths = field.DataLengths();
            const uint8_t* bytes = field.DataBytes();
            if (!offsets || !lengths || !bytes) {
                return false;
            }
            const uint32_t len = lengths[row];
            const uint32_t start = offsets[row];
            *out = std::string(reinterpret_cast<const char*>(bytes + start), len);
            return true;
        }
        return false;
    }

    bool ReadBoolField(const FieldVector& field, size_t row, bool* out) const {
        if (row >= field.Size()) {
            return false;
        }
        if (field.HasNulls() && !field.IsValid(row)) {
            return false;
        }
        if (field.Type() != FieldType::kBool) {
            return false;
        }
        const uint8_t* values = field.DataBool();
        if (!values) {
            return false;
        }
        *out = values[row] != 0;
        return true;
    }

    bool ReadInt64Field(const FieldVector& field, size_t row, int64_t* out) const {
        if (row >= field.Size()) {
            return false;
        }
        if (field.HasNulls() && !field.IsValid(row)) {
            return false;
        }
        if (field.Type() != FieldType::kInt64) {
            return false;
        }
        const int64_t* values = field.DataInt64();
        if (!values) {
            return false;
        }
        *out = values[row];
        return true;
    }

    bool ReadInt32Field(const FieldVector& field, size_t row, int32_t* out) const {
        if (row >= field.Size()) {
            return false;
        }
        if (field.HasNulls() && !field.IsValid(row)) {
            return false;
        }
        if (field.Type() != FieldType::kInt32) {
            return false;
        }
        const int32_t* values = field.DataInt32();
        if (!values) {
            return false;
        }
        *out = values[row];
        return true;
    }

    bool ReadBytesField(const FieldVector& field, size_t row, std::string* out) const {
        if (row >= field.Size()) {
            return false;
        }
        if (field.HasNulls() && !field.IsValid(row)) {
            return false;
        }
        if (field.Type() == FieldType::kBytes) {
            const uint32_t* offsets = field.DataOffsets();
            const uint32_t* lengths = field.DataLengths();
            const uint8_t* bytes = field.DataBytes();
            if (!offsets || !lengths || !bytes) {
                return false;
            }
            const uint32_t len = lengths[row];
            const uint32_t start = offsets[row];
            out->assign(reinterpret_cast<const char*>(bytes + start), len);
            return true;
        }
        return false;
    }

    bool ReadFieldValue(const FieldVector& field, size_t row, FieldValue* out) const {
        if (row >= field.Size()) {
            return false;
        }
        if (field.HasNulls() && !field.IsValid(row)) {
            *out = FieldValue::Null(field.Type());
            return true;
        }
        switch (field.Type()) {
            case FieldType::kInt32: {
                int32_t value = 0;
                if (!ReadInt32Field(field, row, &value)) {
                    return false;
                }
                *out = FieldValue::Int32(value);
                return true;
            }
            case FieldType::kInt64: {
                int64_t value = 0;
                if (!ReadInt64Field(field, row, &value)) {
                    return false;
                }
                *out = FieldValue::Int64(value);
                return true;
            }
            case FieldType::kFloat64: {
                const double* values = field.DataFloat64();
                if (!values) {
                    return false;
                }
                FieldValue val;
                val.type = FieldType::kFloat64;
                val.f64 = values[row];
                *out = std::move(val);
                return true;
            }
            case FieldType::kBool: {
                bool value = false;
                if (!ReadBoolField(field, row, &value)) {
                    return false;
                }
                *out = FieldValue::Bool(value);
                return true;
            }
            case FieldType::kString: {
                std::string value;
                if (!ReadStringField(field, row, &value)) {
                    return false;
                }
                *out = FieldValue::String(value);
                return true;
            }
            case FieldType::kBytes: {
                std::string value;
                if (!ReadBytesField(field, row, &value)) {
                    return false;
                }
                *out = FieldValue::Bytes(value);
                return true;
            }
            case FieldType::kDictInt32: {
                const uint32_t* ids = field.DataDictIds();
                if (!ids) {
                    return false;
                }
                const int32_t value = field.DictionaryValue(ids[row]);
                *out = FieldValue::Int32(value);
                return true;
            }
            default:
                return false;
        }
    }

    bool CollectTailRows(const DatasetState& state, size_t max_rows,
                         std::vector<std::vector<FieldValue>>* out) const {
        if (max_rows == 0) {
            return false;
        }
        std::deque<std::vector<FieldValue>> tail;
        auto append_row = [&](const std::vector<FieldVector>& fields, size_t row) -> bool {
            std::vector<FieldValue> values;
            values.reserve(fields.size());
            for (const auto& field : fields) {
                FieldValue value;
                if (!ReadFieldValue(field, row, &value)) {
                    return false;
                }
                values.push_back(std::move(value));
            }
            if (tail.size() == max_rows) {
                tail.pop_front();
            }
            tail.push_back(std::move(values));
            return true;
        };
        for (const auto& segment : state.dataset->Segments()) {
            const auto& fields = segment.Fields();
            for (size_t row = 0; row < segment.RowCount(); ++row) {
                if (!append_row(fields, row)) {
                    return false;
                }
            }
        }
        const auto& active = state.dataset->ActiveFields();
        const size_t active_rows = state.dataset->ActiveRowCount();
        for (size_t row = 0; row < active_rows; ++row) {
            if (!append_row(active, row)) {
                return false;
            }
        }
        out->assign(tail.begin(), tail.end());
        return true;
    }

    bool CompactAuthDatasetRows(const std::string& dataset_name, size_t max_rows) {
        if (max_rows == 0) {
            return false;
        }
        auto db_it = databases_.find(kAuthDbName);
        if (db_it == databases_.end()) {
            return false;
        }
        auto ds_it = db_it->second.datasets.find(dataset_name);
        if (ds_it == db_it->second.datasets.end()) {
            return false;
        }
        if (ds_it->second.dataset->RowCount() <= max_rows) {
            return false;
        }
        std::vector<std::vector<FieldValue>> rows;
        if (!CollectTailRows(ds_it->second, max_rows, &rows)) {
            return false;
        }
        const auto schema = ds_it->second.dataset->SchemaView();
        std::vector<std::pair<std::string, FieldType>> fields;
        for (const auto& field : schema.Fields()) {
            fields.push_back({field.name, field.type});
        }
        std::error_code ec;
        std::filesystem::remove_all(ds_it->second.path, ec);
        DatasetState new_state;
        new_state.dataset = std::make_unique<Dataset>(dataset_name);
        for (const auto& field : fields) {
            new_state.dataset->AddField(FieldVector(field.first, field.second));
        }
        new_state.path = DatasetPath(kAuthDbName, dataset_name);
        if (!std::filesystem::exists(new_state.path)) {
            std::filesystem::create_directories(new_state.path);
        }
        if (!WriteSchema(new_state.path, *new_state.dataset)) {
            return false;
        }
        for (const auto& row : rows) {
            if (!new_state.dataset->Append(row)) {
                return false;
            }
        }
        if (!PersistNewSegments(kAuthDbName, new_state)) {
            return false;
        }
        db_it->second.datasets[dataset_name] = std::move(new_state);
        return true;
    }

    struct AuthSnapshot {
        std::unordered_map<std::string, bool> key_enabled;
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> key_grants;
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> key_grant_revokes;
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> role_grants;
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> role_grant_revokes;
        std::unordered_map<std::string,
            std::vector<std::pair<std::string, std::string>>> key_roles;
        std::unordered_map<std::string,
            std::vector<std::pair<std::string, std::string>>> key_role_revokes;
    };

    bool LoadAuthSnapshot(AuthSnapshot* out) const {
        const auto db_it = databases_.find(kAuthDbName);
        if (db_it == databases_.end()) {
            return false;
        }
        const auto resolve_index = [](const std::vector<FieldVector>& fields,
                                      const std::string& name,
                                      size_t fallback) -> size_t {
            for (size_t i = 0; i < fields.size(); ++i) {
                if (fields[i].Name() == name) {
                    return i;
                }
            }
            return fallback < fields.size() ? fallback : fields.size();
        };
        const auto load_string = [&](const std::vector<FieldVector>& fields,
                                     size_t row,
                                     size_t index,
                                     std::string* out_value) -> bool {
            if (index >= fields.size()) {
                return false;
            }
            return ReadStringField(fields[index], row, out_value);
        };
        const auto load_bool = [&](const std::vector<FieldVector>& fields,
                                   size_t row,
                                   size_t index,
                                   bool* out_value) -> bool {
            if (index >= fields.size()) {
                return false;
            }
            return ReadBoolField(fields[index], row, out_value);
        };
        const auto scan_dataset = [&](const DatasetState& state,
                                      const std::function<void(const std::vector<FieldVector>&,
                                                               size_t)>& fn) {
            for (const auto& segment : state.dataset->Segments()) {
                const auto& fields = segment.Fields();
                for (size_t row = 0; row < segment.RowCount(); ++row) {
                    fn(fields, row);
                }
            }
            const auto& active = state.dataset->ActiveFields();
            if (!active.empty()) {
                const size_t rows = state.dataset->ActiveRowCount();
                for (size_t row = 0; row < rows; ++row) {
                    fn(active, row);
                }
            }
        };

        auto keys_it = db_it->second.datasets.find("keys");
        if (keys_it != db_it->second.datasets.end()) {
            const auto& schema_fields = keys_it->second.dataset->Fields();
            const size_t fp_idx = resolve_index(schema_fields, "fingerprint", 1);
            const size_t enabled_idx = resolve_index(schema_fields, "enabled", 4);
            scan_dataset(keys_it->second, [&](const std::vector<FieldVector>& fields,
                                              size_t row) {
                std::string fingerprint;
                if (!load_string(fields, row, fp_idx, &fingerprint)) {
                    return;
                }
                bool enabled = true;
                if (!load_bool(fields, row, enabled_idx, &enabled)) {
                    enabled = true;
                }
                out->key_enabled[fingerprint] = enabled;
            });
        }

        auto role_grants_it = db_it->second.datasets.find("role_grants");
        if (role_grants_it != db_it->second.datasets.end()) {
            const auto& schema_fields = role_grants_it->second.dataset->Fields();
            const size_t role_idx = resolve_index(schema_fields, "role_name", 0);
            const size_t cap_idx = resolve_index(schema_fields, "capability", 1);
            const size_t scope_idx = resolve_index(schema_fields, "scope", 2);
            scan_dataset(role_grants_it->second, [&](const std::vector<FieldVector>& fields,
                                                     size_t row) {
                std::string role;
                std::string cap;
                std::string scope;
                if (!load_string(fields, row, role_idx, &role) ||
                    !load_string(fields, row, cap_idx, &cap)) {
                    return;
                }
                if (!load_string(fields, row, scope_idx, &scope)) {
                    scope = "*";
                }
                if (!cap.empty() && cap[0] == '!') {
                    out->role_grant_revokes[role].push_back({cap.substr(1), scope});
                } else {
                    out->role_grants[role].push_back({cap, scope});
                }
            });
        }

        auto key_grants_it = db_it->second.datasets.find("key_grants");
        if (key_grants_it != db_it->second.datasets.end()) {
            const auto& schema_fields = key_grants_it->second.dataset->Fields();
            const size_t fp_idx = resolve_index(schema_fields, "fingerprint", 0);
            const size_t cap_idx = resolve_index(schema_fields, "capability", 1);
            const size_t scope_idx = resolve_index(schema_fields, "scope", 2);
            scan_dataset(key_grants_it->second, [&](const std::vector<FieldVector>& fields,
                                                    size_t row) {
                std::string fp;
                std::string cap;
                std::string scope;
                if (!load_string(fields, row, fp_idx, &fp) ||
                    !load_string(fields, row, cap_idx, &cap)) {
                    return;
                }
                if (!load_string(fields, row, scope_idx, &scope)) {
                    scope = "*";
                }
                if (!cap.empty() && cap[0] == '!') {
                    out->key_grant_revokes[fp].push_back({cap.substr(1), scope});
                } else {
                    out->key_grants[fp].push_back({cap, scope});
                }
            });
        }

        auto key_roles_it = db_it->second.datasets.find("key_roles");
        if (key_roles_it != db_it->second.datasets.end()) {
            const auto& schema_fields = key_roles_it->second.dataset->Fields();
            const size_t fp_idx = resolve_index(schema_fields, "fingerprint", 0);
            const size_t role_idx = resolve_index(schema_fields, "role_name", 1);
            const size_t scope_idx = resolve_index(schema_fields, "scope", 2);
            scan_dataset(key_roles_it->second, [&](const std::vector<FieldVector>& fields,
                                                   size_t row) {
                std::string fp;
                std::string role;
                std::string scope;
                if (!load_string(fields, row, fp_idx, &fp) ||
                    !load_string(fields, row, role_idx, &role)) {
                    return;
                }
                if (!load_string(fields, row, scope_idx, &scope)) {
                    scope = "*";
                }
                if (!role.empty() && role[0] == '!') {
                    out->key_role_revokes[fp].push_back({role.substr(1), scope});
                } else {
                    out->key_roles[fp].push_back({role, scope});
                }
            });
        }

        return true;
    }

    void AddPermission(PermissionSet* perms,
                       const std::string& cap,
                       const std::string& scope) const {
        perms->grants[cap].insert(scope.empty() ? "*" : scope);
        if (cap == "db.admin") {
            static const std::vector<std::string> db_caps = {
                "db.list",
                "db.read",
                "db.write",
                "db.create",
                "db.drop",
                "db.truncate",
            };
            for (const auto& expanded : db_caps) {
                perms->grants[expanded].insert(scope.empty() ? "*" : scope);
            }
        }
    }

    PermissionSet BuildPermissions(const std::string& fingerprint) const {
        PermissionSet perms;
        AuthSnapshot snapshot;
        if (!LoadAuthSnapshot(&snapshot)) {
            return perms;
        }
        auto revoke_grant = [&](const std::string& cap, const std::string& scope) {
            auto it = perms.grants.find(cap);
            if (it == perms.grants.end()) {
                return;
            }
            if (scope.empty() || scope == "*") {
                perms.grants.erase(it);
                return;
            }
            it->second.erase(scope);
            if (it->second.empty()) {
                perms.grants.erase(it);
            }
        };
        auto key_it = snapshot.key_grants.find(fingerprint);
        if (key_it != snapshot.key_grants.end()) {
            for (const auto& grant : key_it->second) {
                AddPermission(&perms, grant.first, grant.second);
            }
        }
        auto key_revoke_it = snapshot.key_grant_revokes.find(fingerprint);
        if (key_revoke_it != snapshot.key_grant_revokes.end()) {
            for (const auto& grant : key_revoke_it->second) {
                revoke_grant(grant.first, grant.second);
            }
        }
        auto roles_it = snapshot.key_roles.find(fingerprint);
        if (roles_it != snapshot.key_roles.end()) {
            for (const auto& role_entry : roles_it->second) {
                const auto role_name = role_entry.first;
                const auto role_scope = role_entry.second;
                auto rg_it = snapshot.role_grants.find(role_name);
                if (rg_it == snapshot.role_grants.end()) {
                    continue;
                }
                for (const auto& grant : rg_it->second) {
                    const std::string scope =
                        CombineScopes(grant.second, role_scope);
                    AddPermission(&perms, grant.first, scope);
                }
                auto rr_it = snapshot.role_grant_revokes.find(role_name);
                if (rr_it != snapshot.role_grant_revokes.end()) {
                    for (const auto& revoke : rr_it->second) {
                        const std::string scope =
                            CombineScopes(revoke.second, role_scope);
                        revoke_grant(revoke.first, scope);
                    }
                }
            }
        }
        auto role_revoke_it = snapshot.key_role_revokes.find(fingerprint);
        if (role_revoke_it != snapshot.key_role_revokes.end()) {
            for (const auto& revoke : role_revoke_it->second) {
                const std::string role_name = revoke.first;
                auto rg_it = snapshot.role_grants.find(role_name);
                if (rg_it == snapshot.role_grants.end()) {
                    continue;
                }
                for (const auto& grant : rg_it->second) {
                    const std::string scope =
                        CombineScopes(grant.second, revoke.second);
                    revoke_grant(grant.first, scope);
                }
            }
        }
        return perms;
    }

    bool IsAuthorized(const PermissionSet& perms,
                      const std::string& capability,
                      const std::string& scope) const {
        auto it = perms.grants.find(capability);
        if (it == perms.grants.end()) {
            return false;
        }
        const auto& scopes = it->second;
        if (scopes.find("*") != scopes.end()) {
            return true;
        }
        if (scopes.find(scope) != scopes.end()) {
            return true;
        }
        if (scope.rfind("dataset:", 0) == 0) {
            const auto dot = scope.find('.', 8);
            if (dot != std::string::npos) {
                const std::string db_scope =
                    "database:" + scope.substr(8, dot - 8);
                if (scopes.find(db_scope) != scopes.end()) {
                    return true;
                }
            }
        }
        return false;
    }
    void EnsureAuthDatabase() {
        auto& auth_db = EnsureDatabase(kAuthDbName);
        const std::vector<std::pair<std::string, FieldType>> keys_fields = {
            {"key_id", FieldType::kString},
            {"fingerprint", FieldType::kString},
            {"public_key", FieldType::kBytes},
            {"comment", FieldType::kString},
            {"enabled", FieldType::kBool},
            {"created_at", FieldType::kInt64},
            {"last_used_at", FieldType::kInt64},
        };
        const std::vector<std::pair<std::string, FieldType>> roles_fields = {
            {"role_name", FieldType::kString},
            {"created_at", FieldType::kInt64},
            {"built_in", FieldType::kBool},
        };
        const std::vector<std::pair<std::string, FieldType>> role_grants_fields = {
            {"role_name", FieldType::kString},
            {"capability", FieldType::kString},
            {"scope", FieldType::kString},
        };
        const std::vector<std::pair<std::string, FieldType>> key_roles_fields = {
            {"fingerprint", FieldType::kString},
            {"role_name", FieldType::kString},
            {"scope", FieldType::kString},
        };
        const std::vector<std::pair<std::string, FieldType>> key_grants_fields = {
            {"fingerprint", FieldType::kString},
            {"capability", FieldType::kString},
            {"scope", FieldType::kString},
        };
        const std::vector<std::pair<std::string, FieldType>> rate_limit_fields = {
            {"remote_addr", FieldType::kString},
            {"fingerprint", FieldType::kString},
            {"fail_count", FieldType::kInt32},
            {"next_allowed_at", FieldType::kInt64},
            {"last_fail_at", FieldType::kInt64},
        };
        const std::vector<std::pair<std::string, FieldType>> audit_log_fields = {
            {"ts", FieldType::kInt64},
            {"event_type", FieldType::kString},
            {"remote_addr", FieldType::kString},
            {"fingerprint", FieldType::kString},
            {"details_json", FieldType::kString},
        };

        CreateDatasetInternal(auth_db, "keys", keys_fields);
        CreateDatasetInternal(auth_db, "roles", roles_fields);
        CreateDatasetInternal(auth_db, "role_grants", role_grants_fields);
        CreateDatasetInternal(auth_db, "key_roles", key_roles_fields);
        CreateDatasetInternal(auth_db, "key_grants", key_grants_fields);
        CreateDatasetInternal(auth_db, "rate_limits", rate_limit_fields);
        CreateDatasetInternal(auth_db, "audit_log", audit_log_fields);

        const std::filesystem::path roles_marker =
            std::filesystem::path(auth_db.path) / "roles_bootstrapped";
        if (std::filesystem::exists(roles_marker)) {
            const std::filesystem::path root_marker =
                std::filesystem::path(auth_db_path_) / "root_initialized";
            const std::filesystem::path root_audit_marker =
                std::filesystem::path(auth_db.path) / "root_init_audited";
            if (std::filesystem::exists(root_marker) &&
                !std::filesystem::exists(root_audit_marker)) {
                AppendAuditLog("root.init", "local", "", "{\"note\":\"root_initialized\"}");
                std::ofstream marker(root_audit_marker, std::ios::binary | std::ios::trunc);
                marker << "ok\n";
            }
            return;
        }
        auto roles_it = auth_db.datasets.find("roles");
        auto grants_it = auth_db.datasets.find("role_grants");
        if (roles_it == auth_db.datasets.end() || grants_it == auth_db.datasets.end()) {
            return;
        }

        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        auto append_role = [&](const std::string& name, bool built_in) {
            roles_it->second.dataset->Append({
                FieldValue::String(name),
                FieldValue::Int64(now),
                FieldValue::Bool(built_in),
            });
        };
        auto append_grant = [&](const std::string& role,
                                const std::string& cap,
                                const std::string& scope) {
            grants_it->second.dataset->Append({
                FieldValue::String(role),
                FieldValue::String(cap),
                FieldValue::String(scope),
            });
        };

        const std::vector<std::string> all_caps = {
            "server.health",
            "server.metrics",
            "server.shutdown",
            "server.config",
            "server.key.rotate",
            "auth.read",
            "auth.manage",
            "auth.assign",
            "db.list",
            "db.read",
            "db.write",
            "db.create",
            "db.drop",
            "db.truncate",
            "db.admin",
            "dataset.list",
            "dataset.read",
            "dataset.write",
            "dataset.create",
            "dataset.drop",
            "dataset.truncate",
            "dataset.schema.read",
            "dataset.schema.modify",
            "query.scan",
            "query.aggregate",
            "query.vector",
            "query.export",
            "query.explain",
        };

        append_role("root", true);
        append_role("admin", true);
        append_role("writer", true);
        append_role("reader", true);
        append_role("restricted-reader", true);

        for (const auto& cap : all_caps) {
            append_grant("root", cap, "*");
        }
        for (const auto& cap : all_caps) {
            if (cap == "auth.manage") {
                continue;
            }
            append_grant("admin", cap, "*");
        }

        const std::vector<std::string> writer_caps = {
            "db.list",
            "dataset.list",
            "dataset.read",
            "dataset.write",
            "query.scan",
            "query.aggregate",
            "query.vector",
        };
        for (const auto& cap : writer_caps) {
            append_grant("writer", cap, "*");
        }

        const std::vector<std::string> reader_caps = {
            "db.list",
            "dataset.list",
            "dataset.read",
            "query.scan",
            "query.aggregate",
            "query.vector",
        };
        for (const auto& cap : reader_caps) {
            append_grant("reader", cap, "*");
        }

        const std::vector<std::string> restricted_caps = {
            "dataset.list",
            "dataset.read",
            "query.scan",
            "query.aggregate",
            "query.vector",
        };
        for (const auto& cap : restricted_caps) {
            append_grant("restricted-reader", cap, "*");
        }

        std::ofstream marker(roles_marker, std::ios::binary | std::ios::trunc);
        marker << "ok\n";
        AppendAuditLog("roles.bootstrap", "local", "", "{\"note\":\"built_in_roles\"}");
    }

    bool WriteSchema(const std::string& dataset_path, const Dataset& dataset) {
        std::ofstream out(std::filesystem::path(dataset_path) / "schema.bin",
                          std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        const auto& fields = dataset.Fields();
        SchemaFileHeader header;
        header.field_count = static_cast<uint16_t>(fields.size());
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        for (const auto& field : fields) {
            const auto& name = field.Name();
            const uint16_t name_len = static_cast<uint16_t>(name.size());
            const uint8_t type = static_cast<uint8_t>(field.Type());
            out.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
            out.write(name.data(), name.size());
            out.write(reinterpret_cast<const char*>(&type), sizeof(type));
        }
        const bool ok = out.good();
        out.close();
        if (!ok) {
            return false;
        }
        if (flush_on_seal_) {
            return FsyncPath(std::filesystem::path(dataset_path) / "schema.bin");
        }
        return true;
    }

    bool ReadSchema(const std::string& dataset_path,
                    std::vector<std::pair<std::string, FieldType>>* fields) {
        std::ifstream in(std::filesystem::path(dataset_path) / "schema.bin", std::ios::binary);
        if (!in.is_open()) {
            return false;
        }
        SchemaFileHeader header;
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in.good() || header.magic != 0x4D435343 || header.version != 1) {
            return false;
        }
        fields->clear();
        fields->reserve(header.field_count);
        for (uint16_t i = 0; i < header.field_count; ++i) {
            uint16_t name_len = 0;
            in.read(reinterpret_cast<char*>(&name_len), sizeof(name_len));
            if (!in.good()) {
                return false;
            }
            std::string name(name_len, '\0');
            in.read(name.data(), name_len);
            uint8_t type = 0;
            in.read(reinterpret_cast<char*>(&type), sizeof(type));
            if (!in.good()) {
                return false;
            }
            fields->push_back({name, DecodeFieldType(type)});
        }
        return true;
    }

    bool PersistNewSegments(const std::string& db_name, DatasetState& state) {
        const auto& segments = state.dataset->Segments();
        while (state.persisted_segments < state.segment_base_index + segments.size()) {
            const size_t index = state.persisted_segments;
            const size_t in_memory_index = index - state.segment_base_index;
            SegmentWriter writer(
                SegmentPath(db_name, state.dataset->Name(), index));
            if (!writer.Write(segments[in_memory_index])) {
                return false;
            }
            if (flush_on_seal_) {
                if (!FsyncPath(SegmentPath(db_name, state.dataset->Name(), index))) {
                    return false;
                }
            }
            state.cached_bytes += SegmentBytes(segments[in_memory_index]);
            state.persisted_segments += 1;
        }
        EnforceSegmentCacheLimits(state);
        return true;
    }

    bool FlushActiveSegments() {
        for (auto& db_entry : databases_) {
            auto& db_name = db_entry.first;
            for (auto& entry : db_entry.second.datasets) {
                auto& state = entry.second;
            const auto& dataset = *state.dataset;
            const size_t active_rows = dataset.ActiveRowCount();
            if (active_rows == 0) {
                continue;
            }
            std::vector<FieldVector> fields = dataset.ActiveFields();
            Segment segment(dataset.SegmentCapacity(), active_rows, std::move(fields));
            SegmentWriter writer(
                SegmentPath(db_name, dataset.Name(), state.persisted_segments));
            if (!writer.Write(segment)) {
                return false;
            }
            state.persisted_segments += 1;
            }
        }
        return true;
    }

    bool FlushDatabaseActiveSegments(const std::string& db_name) {
        auto db_it = databases_.find(db_name);
        if (db_it == databases_.end()) {
            return true;
        }
        for (auto& entry : db_it->second.datasets) {
            auto& state = entry.second;
            const auto& dataset = *state.dataset;
            const size_t active_rows = dataset.ActiveRowCount();
            if (active_rows == 0) {
                continue;
            }
            std::vector<FieldVector> fields = dataset.ActiveFields();
            Segment segment(dataset.SegmentCapacity(), active_rows, std::move(fields));
            SegmentWriter writer(
                SegmentPath(db_name, dataset.Name(), state.persisted_segments));
            if (!writer.Write(segment)) {
                return false;
            }
            state.persisted_segments += 1;
        }
        return true;
    }

    void StartHousekeeping() {
        housekeeping_running_.store(true);
        housekeeping_thread_ = std::thread([this]() {
            while (housekeeping_running_.load()) {
                if (active_clients_.load() == 0) {
                    RunMaintenanceTasks();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });
    }

    void StopHousekeeping() {
        housekeeping_running_.store(false);
        if (housekeeping_thread_.joinable()) {
            housekeeping_thread_.join();
        }
    }

    void RunMaintenanceTasks() {
        // Placeholder for recovery/maintenance scheduling hooks.
        PruneRateLimitState();
        RunAuthRetentionTasks();
        PruneSessions();
        PruneHandshakeNonces();
        if (flush_interval_ms_ > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (last_active_flush_.time_since_epoch().count() == 0 ||
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_active_flush_)
                        .count() >= flush_interval_ms_) {
                FlushActiveSegments();
                last_active_flush_ = now;
            }
        }
    }

    void RecoverDatabaseAtPath(const std::string& db_name,
                               const std::filesystem::path& db_path) {
        if (!std::filesystem::exists(db_path) || !std::filesystem::is_directory(db_path)) {
            return;
        }
        auto& db_state = EnsureDatabase(db_name);
        db_state.path = db_path.string();
        for (const auto& dataset_entry : std::filesystem::directory_iterator(db_path)) {
            if (!dataset_entry.is_directory()) {
                continue;
            }
            const auto dataset_name = dataset_entry.path().filename().string();
            std::vector<std::pair<std::string, FieldType>> fields;
            if (!ReadSchema(dataset_entry.path().string(), &fields)) {
                continue;
            }
            auto dataset = std::make_unique<Dataset>(dataset_name);
            for (const auto& field : fields) {
                dataset->AddField(FieldVector(field.first, field.second));
            }
            DatasetState state;
            state.dataset = std::move(dataset);
            state.path = dataset_entry.path().string();
            std::vector<std::filesystem::path> segments;
            for (const auto& file : std::filesystem::directory_iterator(dataset_entry.path())) {
                if (!file.is_regular_file()) {
                    continue;
                }
                if (file.path().extension() == ".mimicdb") {
                    segments.push_back(file.path());
                }
            }
            std::sort(segments.begin(), segments.end());
            for (const auto& segment_path : segments) {
                SegmentReader reader(segment_path.string());
                Segment segment(0, {});
                if (!reader.ReadWithSchema(&segment, state.dataset->SchemaView())) {
                    std::error_code ec;
                    std::filesystem::remove(segment_path, ec);
                    continue;
                }
                if (!state.dataset->AddRecoveredSegment(std::move(segment))) {
                    continue;
                }
                state.persisted_segments += 1;
                state.cached_bytes += SegmentBytes(state.dataset->Segments().back());
                EnforceSegmentCacheLimits(state);
            }
            db_state.datasets[dataset_name] = std::move(state);
        }
    }

    void RecoverDatasets() {
        if (storage_root_.empty()) {
            storage_root_ = "./data";
        }
        std::filesystem::create_directories(storage_root_);
        const std::string default_auth_path =
            (std::filesystem::path(storage_root_) / kAuthDbName).string();
        for (const auto& db_entry : std::filesystem::directory_iterator(storage_root_)) {
            if (!db_entry.is_directory()) {
                continue;
            }
            const auto db_name = db_entry.path().filename().string();
            if (IsAuthDatabaseName(db_name) && !auth_db_path_.empty() &&
                auth_db_path_ != default_auth_path) {
                continue;
            }
            RecoverDatabaseAtPath(db_name, db_entry.path());
        }
        if (!auth_db_path_.empty() && auth_db_path_ != default_auth_path) {
            RecoverDatabaseAtPath(kAuthDbName, auth_db_path_);
        }
    }
};

Server* Server::instance_ = nullptr;

}  // namespace mimicdb

struct ServerConfig {
    std::string bind = "127.0.0.1:9000";
    std::string storage_root = "./data";
    std::string auth_db_path = "";
    std::string host_key_path = "";
    uint32_t session_idle_timeout_sec = 900;
    uint32_t session_max_lifetime_sec = 86400;
    size_t session_max_total = 10000;
    size_t session_max_per_fingerprint = 10;
    uint32_t handshake_nonce_ttl_sec = 300;
    size_t handshake_nonce_max_entries = 10000;
    int auth_failure_delay_ms = 25;
    uint32_t auth_rate_limit_burst = 3;
    std::string auth_rate_limit_backoff_sec = "30,60,120,240,480";
    size_t auth_rate_limit_state_max_entries = 10000;
    uint32_t auth_rate_limit_state_ttl_sec = 3600;
    size_t auth_rate_limit_max_rows = 50000;
    size_t auth_audit_log_max_rows = 100000;
    size_t auth_prune_batch_rows = 1000;
    bool flush_on_shutdown = false;
    bool flush_on_seal = true;
    int flush_interval_ms = 0;
    uint32_t max_payload_bytes = 268435456;
    uint32_t max_rows_per_batch = 5000000;
    int append_sleep_ms = 0;
    size_t segment_cache_max = 2;
    uint64_t segment_cache_bytes = 20ULL * 1024ULL * 1024ULL * 1024ULL;
    size_t query_threads = 16;
    bool compression_enabled = true;
    bool compression_enable_dict = true;
    bool compression_enable_bitpack = true;
    bool compression_enable_fordelta = true;
    bool compression_enable_lz4 = true;
    size_t compression_min_segment_rows = 4096;
    double compression_min_ratio = 1.1;
    bool compression_metrics = false;
};

std::string Trim(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

bool ParseBool(const std::string& value, bool* out) {
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        *out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        *out = false;
        return true;
    }
    return false;
}

bool LoadConfig(const std::string& path, ServerConfig* config) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        const auto pos = trimmed.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, pos));
        const std::string value = Trim(trimmed.substr(pos + 1));
        if (key == "bind") {
            config->bind = value;
        } else if (key == "storage_root") {
            config->storage_root = value;
        } else if (key == "auth_db_path") {
            config->auth_db_path = value;
        } else if (key == "host_key_path") {
            config->host_key_path = value;
        } else if (key == "session_idle_timeout_sec") {
            try {
                config->session_idle_timeout_sec = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "session_max_lifetime_sec") {
            try {
                config->session_max_lifetime_sec = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "session_max_total") {
            try {
                config->session_max_total = static_cast<size_t>(std::stoull(value));
            } catch (...) {
            }
        } else if (key == "session_max_per_fingerprint") {
            try {
                config->session_max_per_fingerprint = static_cast<size_t>(std::stoull(value));
            } catch (...) {
            }
        } else if (key == "handshake_nonce_ttl_sec") {
            try {
                config->handshake_nonce_ttl_sec = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "handshake_nonce_max_entries") {
            try {
                config->handshake_nonce_max_entries = static_cast<size_t>(std::stoull(value));
            } catch (...) {
            }
        } else if (key == "auth_failure_delay_ms") {
            try {
                config->auth_failure_delay_ms = std::stoi(value);
            } catch (...) {
            }
        } else if (key == "auth_rate_limit_burst") {
            try {
                config->auth_rate_limit_burst = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "auth_rate_limit_backoff_sec") {
            config->auth_rate_limit_backoff_sec = value;
        } else if (key == "auth_rate_limit_state_max_entries") {
            try {
                config->auth_rate_limit_state_max_entries = static_cast<size_t>(std::stoull(value));
            } catch (...) {
            }
        } else if (key == "auth_rate_limit_state_ttl_sec") {
            try {
                config->auth_rate_limit_state_ttl_sec = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "auth_rate_limit_max_rows") {
            try {
                config->auth_rate_limit_max_rows = static_cast<size_t>(std::stoull(value));
            } catch (...) {
            }
        } else if (key == "auth_audit_log_max_rows") {
            try {
                config->auth_audit_log_max_rows = static_cast<size_t>(std::stoull(value));
            } catch (...) {
            }
        } else if (key == "auth_prune_batch_rows") {
            try {
                config->auth_prune_batch_rows = static_cast<size_t>(std::stoull(value));
            } catch (...) {
            }
        } else if (key == "flush_on_shutdown") {
            bool parsed = false;
            if (ParseBool(value, &parsed)) {
                config->flush_on_shutdown = parsed;
            }
        } else if (key == "flush_on_seal") {
            bool parsed = false;
            if (ParseBool(value, &parsed)) {
                config->flush_on_seal = parsed;
            }
        } else if (key == "flush_interval_ms") {
            try {
                config->flush_interval_ms = std::stoi(value);
            } catch (...) {
            }
        } else if (key == "max_payload_bytes") {
            try {
                config->max_payload_bytes = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "max_rows_per_batch") {
            try {
                config->max_rows_per_batch = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "append_sleep_ms") {
            try {
                config->append_sleep_ms = std::stoi(value);
            } catch (...) {
            }
        } else if (key == "segment_cache_max") {
            try {
                config->segment_cache_max = static_cast<size_t>(std::stoull(value));
            } catch (...) {
            }
        } else if (key == "segment_cache_bytes") {
            try {
                config->segment_cache_bytes = static_cast<uint64_t>(std::stoull(value));
            } catch (...) {
            }
        } else if (key == "query_threads") {
            try {
                config->query_threads = static_cast<size_t>(std::stoull(value));
            } catch (...) {
            }
        } else if (key == "compression_enabled") {
            bool parsed = false;
            if (ParseBool(value, &parsed)) {
                config->compression_enabled = parsed;
            }
        } else if (key == "compression_enable_dict") {
            bool parsed = false;
            if (ParseBool(value, &parsed)) {
                config->compression_enable_dict = parsed;
            }
        } else if (key == "compression_enable_bitpack") {
            bool parsed = false;
            if (ParseBool(value, &parsed)) {
                config->compression_enable_bitpack = parsed;
            }
        } else if (key == "compression_enable_fordelta") {
            bool parsed = false;
            if (ParseBool(value, &parsed)) {
                config->compression_enable_fordelta = parsed;
            }
        } else if (key == "compression_enable_lz4") {
            bool parsed = false;
            if (ParseBool(value, &parsed)) {
                config->compression_enable_lz4 = parsed;
            }
        } else if (key == "compression_min_segment_rows") {
            try {
                config->compression_min_segment_rows = static_cast<size_t>(std::stoull(value));
            } catch (...) {
            }
        } else if (key == "compression_min_ratio") {
            try {
                config->compression_min_ratio = std::stod(value);
            } catch (...) {
            }
        } else if (key == "compression_metrics") {
            bool parsed = false;
            if (ParseBool(value, &parsed)) {
                config->compression_metrics = parsed;
            }
        }
    }
    return true;
}

bool ParseBind(const std::string& bind, std::string* host, uint16_t* port) {
    if (bind.empty()) {
        return false;
    }
    const auto pos = bind.rfind(':');
    if (pos == std::string::npos) {
        try {
            const int parsed = std::stoi(bind);
            if (parsed <= 0 || parsed > 65535) {
                return false;
            }
            *host = "127.0.0.1";
            *port = static_cast<uint16_t>(parsed);
            return true;
        } catch (...) {
            return false;
        }
    }
    const std::string host_part = bind.substr(0, pos);
    const std::string port_part = bind.substr(pos + 1);
    if (port_part.empty()) {
        return false;
    }
    try {
        const int parsed = std::stoi(port_part);
        if (parsed <= 0 || parsed > 65535) {
            return false;
        }
        if (host_part.empty() || host_part == "localhost") {
            *host = "127.0.0.1";
        } else {
            *host = host_part;
        }
        *port = static_cast<uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<uint32_t> ParseBackoffSchedule(const std::string& value) {
    std::vector<uint32_t> out;
    size_t start = 0;
    while (start < value.size()) {
        size_t end = value.find(',', start);
        if (end == std::string::npos) {
            end = value.size();
        }
        const std::string token = Trim(value.substr(start, end - start));
        if (!token.empty()) {
            try {
                const int parsed = std::stoi(token);
                if (parsed > 0) {
                    out.push_back(static_cast<uint32_t>(parsed));
                }
            } catch (...) {
            }
        }
        start = end + 1;
    }
    return out;
}

bool IsLocalBind(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost";
}

bool IsRootInitialized(const std::string& auth_db_path) {
    if (auth_db_path.empty()) {
        return false;
    }
    std::filesystem::path marker = std::filesystem::path(auth_db_path) / "root_initialized";
    std::error_code ec;
    return std::filesystem::exists(marker, ec);
}

namespace mimicdb {

bool FsyncPath(const std::filesystem::path& path) {
#if defined(__unix__) || defined(__APPLE__)
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const int result = ::fsync(fd);
    ::close(fd);
    return result == 0;
#else
    (void)path;
    return true;
#endif
}

}  // namespace mimicdb

int main(int argc, char** argv) {
    std::string config_path = "./mimicdb.conf";
    if (const char* env_config = std::getenv("MIMICDB_CONFIG")) {
        config_path = env_config;
    }
    bool rotate_host_key = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--rotate-host-key") {
            rotate_host_key = true;
        }
    }
    if (argc > 2 && (std::string(argv[1]) == "--config" || std::string(argv[1]) == "-c")) {
        config_path = argv[2];
    }

    ServerConfig config;
    LoadConfig(config_path, &config);
    mimicdb::CompressionConfig compression;
    compression.enabled = config.compression_enabled;
    compression.enable_dictionary = config.compression_enable_dict;
    compression.enable_bitpack = config.compression_enable_bitpack;
    compression.enable_fordelta = config.compression_enable_fordelta;
    compression.enable_lz4 = config.compression_enable_lz4;
    compression.min_segment_rows = config.compression_min_segment_rows;
    compression.min_ratio = config.compression_min_ratio;
    mimicdb::SetCompressionConfig(compression);
    std::string bind_host = "127.0.0.1";
    uint16_t port = 9000;
    if (!ParseBind(config.bind, &bind_host, &port)) {
        std::cerr << "invalid bind config, using 127.0.0.1:9000\n";
        bind_host = "127.0.0.1";
        port = 9000;
    }
    if (config.auth_db_path.empty()) {
        config.auth_db_path =
            (std::filesystem::path(config.storage_root) / "__auth__").string();
    }
    if (config.host_key_path.empty()) {
        config.host_key_path =
            (std::filesystem::path(config.auth_db_path) / "host_key").string();
    }
    if (!IsRootInitialized(config.auth_db_path) && !IsLocalBind(bind_host)) {
        std::cerr << "root not initialized: refusing non-local bind " << bind_host << "\n";
        return 1;
    }
    std::array<uint8_t, mimicdb::kEd25519PrivBytes> host_priv{};
    std::array<uint8_t, mimicdb::kHostKeyBytes> host_pub{};
    std::string host_key_fingerprint;
    if (!mimicdb::EnsureHostKey(config.host_key_path, rotate_host_key, &host_priv,
                       &host_pub, &host_key_fingerprint)) {
        std::cerr << "failed to load or generate host key at " << config.host_key_path << "\n";
        return 1;
    }
    std::cerr << "host_key_fingerprint=" << host_key_fingerprint << "\n";
    std::cerr << "host_key_hex=" << mimicdb::HexEncode(host_pub.data(), host_pub.size()) << "\n";
    if (argc == 2 && std::string(argv[1]).rfind("-", 0) != 0) {
        try {
            const int parsed = std::stoi(argv[1]);
            if (parsed > 0 && parsed <= 65535) {
                port = static_cast<uint16_t>(parsed);
            }
        } catch (...) {
        }
    }
    std::vector<uint8_t> host_key_bytes(host_pub.begin(), host_pub.end());
    std::vector<uint32_t> backoff_schedule =
        ParseBackoffSchedule(config.auth_rate_limit_backoff_sec);
    mimicdb::Server server(bind_host, port, config.storage_root, config.auth_db_path,
                        config.host_key_path,
                        host_priv, std::move(host_key_bytes), host_key_fingerprint,
                        config.session_idle_timeout_sec,
                        config.session_max_lifetime_sec,
                        config.session_max_total,
                        config.session_max_per_fingerprint,
                        config.handshake_nonce_ttl_sec,
                        config.handshake_nonce_max_entries,
                        config.auth_failure_delay_ms,
                        config.auth_rate_limit_burst,
                        std::move(backoff_schedule),
                        config.auth_rate_limit_state_max_entries,
                        config.auth_rate_limit_state_ttl_sec,
                        config.auth_rate_limit_max_rows,
                        config.auth_audit_log_max_rows,
                        config.auth_prune_batch_rows,
                        config.flush_on_shutdown, config.flush_on_seal,
                        config.flush_interval_ms, config.max_payload_bytes,
                        config.max_rows_per_batch, config.append_sleep_ms,
                        config.segment_cache_max, config.segment_cache_bytes,
                        config.query_threads, config.compression_metrics);
    return server.Run();
}
