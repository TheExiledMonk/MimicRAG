#include <arpa/inet.h>
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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <csignal>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pcdb/aggregate.h"
#include "pcdb/dataset.h"
#include "pcdb/field_vector.h"
#include "pcdb/segment_io.h"
#include "pcdb/types.h"

namespace pcdb {
namespace {

constexpr uint32_t kMagic = 0x50434442;  // "PCDB"
constexpr uint16_t kVersion = 1;

enum class OpCode : uint16_t {
    kPing = 1,
    kCreateDataset = 2,
    kAppendBatch = 3,
    kQueryAgg = 4,
    kHealth = 5,
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
};

struct SchemaFileHeader {
    uint32_t magic = 0x50435343;  // "PCSC"
    uint32_t version = 1;
    uint16_t field_count = 0;
    uint16_t reserved = 0;
};

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
    }
    return 0;
}

FieldType DecodeFieldType(uint8_t type) {
    return static_cast<FieldType>(type);
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
           bool flush_on_shutdown)
        : bind_addr_(std::move(bind_addr)),
          port_(port),
          storage_root_(std::move(storage_root)),
          flush_on_shutdown_(flush_on_shutdown) {}

    int Run() {
        running_.store(true);
        instance_ = this;
        std::signal(SIGINT, &HandleSignal);
        std::signal(SIGTERM, &HandleSignal);
        RecoverDatasets();
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
        std::cout << "pcdb_server listening on " << port_ << "\n";
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
            case OpCode::kCreateDataset:
                HandleCreateDataset(client, header, payload);
                return;
            case OpCode::kAppendBatch:
                HandleAppendBatch(client, header, payload);
                return;
            case OpCode::kQueryAgg:
                HandleQueryAgg(client, header, payload);
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
        uint16_t name_len = 0;
        if (!ReadScalar(payload, &cursor, &name_len)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::string name;
        if (!ReadBytes(payload, &cursor, name_len, &name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        uint16_t field_count = 0;
        if (!ReadScalar(payload, &cursor, &field_count)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        if (datasets_.find(name) != datasets_.end()) {
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
        state.path = DatasetPath(name);
        if (!std::filesystem::exists(state.path)) {
            std::filesystem::create_directories(state.path);
        }
        if (!WriteSchema(state.path, *state.dataset)) {
            SendStatus(client, header, Status::kInternalError, {});
            return;
        }
        datasets_[name] = std::move(state);
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleAppendBatch(int client, const MessageHeader& header,
                           const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        uint16_t name_len = 0;
        if (!ReadScalar(payload, &cursor, &name_len)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::string name;
        if (!ReadBytes(payload, &cursor, name_len, &name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        auto it = datasets_.find(name);
        if (it == datasets_.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        uint32_t row_count = 0;
        uint16_t field_count = 0;
        if (!ReadScalar(payload, &cursor, &row_count) ||
            !ReadScalar(payload, &cursor, &field_count)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::vector<FieldBatch> batches(field_count);
        std::vector<std::vector<uint8_t>> batch_storage(field_count);
        std::vector<std::vector<uint8_t>> validity_storage(field_count);
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
            const size_t type_size = TypeSize(type);
            const size_t data_bytes = static_cast<size_t>(element_count) * type_size;
            if (cursor + data_bytes > payload.size()) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            batch_storage[field_index].resize(data_bytes);
            std::memcpy(batch_storage[field_index].data(), payload.data() + cursor, data_bytes);
            cursor += data_bytes;

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
                batch_storage[field_index].data(),
                element_count,
                validity_ptr,
            };
        }
        if (!it->second.dataset->AppendBatch(batches)) {
            SendStatus(client, header, Status::kInternalError, {});
            return;
        }
        if (!PersistNewSegments(it->second)) {
            SendStatus(client, header, Status::kInternalError, {});
            return;
        }
        SendStatus(client, header, Status::kOk, {});
    }

    void HandleQueryAgg(int client, const MessageHeader& header,
                        const std::vector<uint8_t>& payload) {
        size_t cursor = 0;
        uint16_t name_len = 0;
        if (!ReadScalar(payload, &cursor, &name_len)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        std::string name;
        if (!ReadBytes(payload, &cursor, name_len, &name)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        auto it = datasets_.find(name);
        if (it == datasets_.end()) {
            SendStatus(client, header, Status::kNotFound, {});
            return;
        }
        uint16_t field_index = 0;
        if (!ReadScalar(payload, &cursor, &field_index)) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        const Dataset& dataset = *it->second.dataset;
        AggregateResult result{};
        for (const auto& segment : dataset.Segments()) {
            if (field_index >= segment.Fields().size()) {
                SendStatus(client, header, Status::kBadRequest, {});
                return;
            }
            AggregateResult partial{};
            AggregateMixed(segment.Fields()[field_index], nullptr, &partial);
            MergeAggregate(partial, &result);
        }
        if (field_index >= dataset.ActiveFields().size()) {
            SendStatus(client, header, Status::kBadRequest, {});
            return;
        }
        AggregateResult partial{};
        AggregateMixed(dataset.ActiveFields()[field_index], nullptr, &partial);
        MergeAggregate(partial, &result);

        std::vector<uint8_t> out(sizeof(uint64_t) + sizeof(double) * 3 + sizeof(uint8_t));
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

        SendStatus(client, header, Status::kOk, out);
    }

    void HandleHealth(int client, const MessageHeader& header) {
        uint16_t dataset_count = static_cast<uint16_t>(datasets_.size());
        uint64_t segment_count = 0;
        uint64_t row_count = 0;
        for (const auto& entry : datasets_) {
            const auto& dataset = *entry.second.dataset;
            segment_count += dataset.Segments().size();
            row_count += dataset.RowCount();
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
    std::unordered_map<std::string, DatasetState> datasets_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> active_clients_{0};
    std::atomic<bool> housekeeping_running_{false};
    std::thread housekeeping_thread_;
    static Server* instance_;

    std::string DatasetPath(const std::string& name) const {
        return (std::filesystem::path(storage_root_) / name).string();
    }

    std::string SchemaPath(const std::string& name) const {
        return (std::filesystem::path(DatasetPath(name)) / "schema.bin").string();
    }

    std::string SegmentPath(const std::string& name, size_t index) const {
        return (std::filesystem::path(DatasetPath(name)) /
                ("segment_" + std::to_string(index) + ".pcdb"))
            .string();
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
        return out.good();
    }

    bool ReadSchema(const std::string& dataset_path, std::vector<std::pair<std::string, FieldType>>* fields) {
        std::ifstream in(std::filesystem::path(dataset_path) / "schema.bin", std::ios::binary);
        if (!in.is_open()) {
            return false;
        }
        SchemaFileHeader header;
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in.good() || header.magic != 0x50435343 || header.version != 1) {
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

    bool PersistNewSegments(DatasetState& state) {
        const auto& segments = state.dataset->Segments();
        while (state.persisted_segments < segments.size()) {
            const size_t index = state.persisted_segments;
            SegmentWriter writer(SegmentPath(state.dataset->Name(), index));
            if (!writer.Write(segments[index])) {
                return false;
            }
            state.persisted_segments += 1;
        }
        return true;
    }

    bool FlushActiveSegments() {
        for (auto& entry : datasets_) {
            auto& state = entry.second;
            const auto& dataset = *state.dataset;
            const size_t active_rows = dataset.ActiveRowCount();
            if (active_rows == 0) {
                continue;
            }
            std::vector<FieldVector> fields = dataset.ActiveFields();
            Segment segment(dataset.SegmentCapacity(), active_rows, std::move(fields));
            SegmentWriter writer(SegmentPath(dataset.Name(), state.persisted_segments));
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
        if (flush_on_shutdown_) {
            FlushActiveSegments();
        }
    }

    void RecoverDatasets() {
        if (storage_root_.empty()) {
            storage_root_ = "./data";
        }
        std::filesystem::create_directories(storage_root_);
        for (const auto& entry : std::filesystem::directory_iterator(storage_root_)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto dataset_name = entry.path().filename().string();
            std::vector<std::pair<std::string, FieldType>> fields;
            if (!ReadSchema(entry.path().string(), &fields)) {
                continue;
            }
            auto dataset = std::make_unique<Dataset>(dataset_name);
            for (const auto& field : fields) {
                dataset->AddField(FieldVector(field.first, field.second));
            }
            DatasetState state;
            state.dataset = std::move(dataset);
            state.path = entry.path().string();
            std::vector<std::filesystem::path> segments;
            for (const auto& file : std::filesystem::directory_iterator(entry.path())) {
                if (!file.is_regular_file()) {
                    continue;
                }
                if (file.path().extension() == ".pcdb") {
                    segments.push_back(file.path());
                }
            }
            std::sort(segments.begin(), segments.end());
            for (const auto& segment_path : segments) {
                SegmentReader reader(segment_path.string());
                Segment segment(0, {});
                if (!reader.ReadWithSchema(&segment, state.dataset->SchemaView())) {
                    continue;
                }
                if (!state.dataset->AddRecoveredSegment(std::move(segment))) {
                    continue;
                }
                state.persisted_segments += 1;
            }
            datasets_[dataset_name] = std::move(state);
        }
    }
};

Server* Server::instance_ = nullptr;

}  // namespace
}  // namespace pcdb

struct ServerConfig {
    std::string bind = "127.0.0.1:9000";
    std::string storage_root = "./data";
    bool flush_on_shutdown = false;
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

int main(int argc, char** argv) {
    std::string config_path = "./pcdb.conf";
    if (const char* env_config = std::getenv("PCDB_CONFIG")) {
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
    pcdb::Server server(bind_host, port, config.storage_root, config.flush_on_shutdown);
    return server.Run();
}
