#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <csignal>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mimicdb/aggregate.h"
#include "mimicdb/dataset.h"
#include "mimicdb/field_vector.h"
#include "mimicdb/segment_io.h"
#include "mimicdb/mask.h"
#include "mimicdb/predicate.h"
#include "mimicdb/types.h"

namespace mimicdb {
namespace {

constexpr uint32_t kMagic = 0x4D434442;  // "MimicDB"
constexpr uint16_t kVersion = 1;

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
};

enum class Status : uint16_t {
    kOk = 0,
    kBadRequest = 1,
    kNotFound = 2,
    kInternalError = 3,
    kUnsupported = 4,
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
           bool flush_on_shutdown, bool flush_on_seal, int flush_interval_ms,
           uint32_t max_payload_bytes, uint32_t max_rows_per_batch, int append_sleep_ms,
           size_t segment_cache_max, uint64_t segment_cache_bytes, size_t query_threads)
        : bind_addr_(std::move(bind_addr)),
          port_(port),
          storage_root_(std::move(storage_root)),
          flush_on_shutdown_(flush_on_shutdown),
          flush_on_seal_(flush_on_seal),
          flush_interval_ms_(flush_interval_ms),
          max_payload_bytes_(max_payload_bytes),
          max_rows_per_batch_(max_rows_per_batch),
          append_sleep_ms_(append_sleep_ms),
          segment_cache_max_(segment_cache_max),
          segment_cache_bytes_(segment_cache_bytes),
          query_threads_(query_threads) {}

    int Run() {
        running_.store(true);
        instance_ = this;
        std::signal(SIGINT, &HandleSignal);
        std::signal(SIGTERM, &HandleSignal);
        RecoverDatasets();
        EnsureDatabase("default");
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
            HandleClient(client);
            active_clients_.fetch_sub(1);
            ::close(client);
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

    void HandleClient(int client) {
        for (;;) {
            MessageHeader header{};
            if (!ReadExact(client, &header, sizeof(header))) {
                return;
            }
            if (header.magic != kMagic || header.version != kVersion) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            if (max_payload_bytes_ > 0 && header.payload_size > max_payload_bytes_) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            std::vector<uint8_t> payload(header.payload_size);
            if (header.payload_size > 0) {
                if (!ReadExact(client, payload.data(), payload.size())) {
                    return;
                }
            }
            Dispatch(client, header, payload);
        }
    }

    void Dispatch(int client, const MessageHeader& header,
                  const std::vector<uint8_t>& payload) {
        const OpCode opcode = static_cast<OpCode>(header.opcode);
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
            case OpCode::kScan:
                HandleScan(client, header, payload);
                return;
            case OpCode::kHealth:
                HandleHealth(client, header);
                return;
            default:
                SendStatus(client, header, Status::kUnsupported, {});
                return;
        }
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
            if (type == FieldType::kString || type == FieldType::kBytes) {
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
                validity_storage[field_index].resize(validity_bytes);
                std::memcpy(validity_storage[field_index].data(), payload.data() + cursor,
                            validity_bytes);
                cursor += validity_bytes;
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

        std::vector<OutputColumn> outputs;
        outputs.reserve(column_indices.size());
        for (uint16_t idx : column_indices) {
            OutputColumn out;
            out.index = idx;
            out.type = dataset.Fields()[idx].Type();
            outputs.push_back(std::move(out));
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
            }
        };

        uint64_t remaining_limit = limit;
        uint64_t remaining_offset = offset;
        bool stop = false;

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

        if (!ForEachSegment(
                db_name, it->second,
                [&](const Segment& segment) -> bool {
                    if (!scan_fields(segment.Fields(), segment.RowCount())) {
                        return false;
                    }
                    return !stop;
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

        SendStatus(client, header, Status::kOk, out);
    }

    void HandleHealth(int client, const MessageHeader& header) {
        uint16_t dataset_count = 0;
        uint64_t segment_count = 0;
        uint64_t row_count = 0;
        for (const auto& db_entry : databases_) {
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

    void HandleCreateDatabase(int client, const MessageHeader& header,
                              const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        std::string name;
        if (!ReadName(payload, &cursor, &name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        EnsureDatabase(name);
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleListDatabases(int client, const MessageHeader& header) {
        std::vector<uint8_t> out;
        const uint16_t count = static_cast<uint16_t>(
            std::min<size_t>(databases_.size(), UINT16_MAX));
        out.resize(sizeof(uint16_t));
        std::memcpy(out.data(), &count, sizeof(uint16_t));
        size_t written = 0;
        for (const auto& entry : databases_) {
            if (written >= count) {
                break;
            }
            const auto& name = entry.first;
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
        WriteExact(client, &response, sizeof(response));
        if (!payload.empty()) {
            WriteStream(client, payload.data(), payload.size(), 64 * 1024);
        }
    }

    std::string bind_addr_;
    uint16_t port_ = 0;
    std::string storage_root_;
    bool flush_on_shutdown_ = false;
    bool flush_on_seal_ = true;
    int flush_interval_ms_ = 0;
    uint32_t max_payload_bytes_ = 0;
    uint32_t max_rows_per_batch_ = 0;
    int append_sleep_ms_ = 0;
    size_t segment_cache_max_ = 0;
    uint64_t segment_cache_bytes_ = 0;
    size_t query_threads_ = 1;
    std::unordered_map<std::string, DatabaseState> databases_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> active_clients_{0};
    std::atomic<bool> housekeeping_running_{false};
    std::thread housekeeping_thread_;
    static Server* instance_;
    std::chrono::steady_clock::time_point last_active_flush_{};

    std::string DatabasePath(const std::string& name) const {
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

    void RecoverDatasets() {
        if (storage_root_.empty()) {
            storage_root_ = "./data";
        }
        std::filesystem::create_directories(storage_root_);
        for (const auto& db_entry : std::filesystem::directory_iterator(storage_root_)) {
            if (!db_entry.is_directory()) {
                continue;
            }
            const auto db_name = db_entry.path().filename().string();
            auto& db_state = EnsureDatabase(db_name);
            for (const auto& dataset_entry : std::filesystem::directory_iterator(db_entry.path())) {
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
    }
};

Server* Server::instance_ = nullptr;

}  // namespace
}  // namespace mimicdb

struct ServerConfig {
    std::string bind = "127.0.0.1:9000";
    std::string storage_root = "./data";
    bool flush_on_shutdown = false;
    bool flush_on_seal = true;
    int flush_interval_ms = 0;
    uint32_t max_payload_bytes = 268435456;
    uint32_t max_rows_per_batch = 5000000;
    int append_sleep_ms = 0;
    size_t segment_cache_max = 2;
    uint64_t segment_cache_bytes = 20ULL * 1024ULL * 1024ULL * 1024ULL;
    size_t query_threads = 16;
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
        *host = host_part.empty() ? "127.0.0.1" : host_part;
        *port = static_cast<uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

namespace mimicdb {
namespace {

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

}  // namespace
}  // namespace mimicdb

int main(int argc, char** argv) {
    std::string config_path = "./mimicdb.conf";
    if (const char* env_config = std::getenv("MIMICDB_CONFIG")) {
        config_path = env_config;
    }
    if (argc > 2 && (std::string(argv[1]) == "--config" || std::string(argv[1]) == "-c")) {
        config_path = argv[2];
    }

    ServerConfig config;
    LoadConfig(config_path, &config);
    std::string bind_host = "127.0.0.1";
    uint16_t port = 9000;
    if (!ParseBind(config.bind, &bind_host, &port)) {
        std::cerr << "invalid bind config, using 127.0.0.1:9000\n";
        bind_host = "127.0.0.1";
        port = 9000;
    }
    if (argc == 2 && std::string(argv[1]).rfind("-", 0) != 0) {
        try {
            const int parsed = std::stoi(argv[1]);
            if (parsed > 0 && parsed <= 65535) {
                port = static_cast<uint16_t>(parsed);
            }
        } catch (...) {
        }
    }
    mimicdb::Server server(bind_host, port, config.storage_root, config.flush_on_shutdown,
                        config.flush_on_seal, config.flush_interval_ms,
                        config.max_payload_bytes, config.max_rows_per_batch,
                        config.append_sleep_ms, config.segment_cache_max,
                        config.segment_cache_bytes, config.query_threads);
    return server.Run();
}
