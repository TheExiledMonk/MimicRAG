#include "mimicdb/vector_gpu.h"

#if MIMICDB_HAS_VULKAN

#include <shaderc/shaderc.hpp>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

#include "mimicdb/dataset.h"

namespace mimicdb {
namespace {

constexpr char kShader[] = R"glsl(
#version 450
layout(local_size_x = 128) in;
layout(set=0,binding=0,std430) readonly buffer Vectors { float vectors[]; };
layout(set=0,binding=1,std430) readonly buffer Query { float query[]; };
layout(set=0,binding=2,std430) readonly buffer Candidates { uint candidates[]; };
layout(set=0,binding=3,std430) readonly buffer Valid { uint valid[]; };
layout(set=0,binding=4,std430) writeonly buffer Output { float distances[]; };
layout(push_constant) uniform Params { uint dimension; uint count; uint metric; uint sparse; uint rows; float query_norm; } p;
void main() {
    uint out_id = gl_GlobalInvocationID.x;
    if (out_id >= p.count) return;
    uint row = p.sparse != 0 ? candidates[out_id] : out_id;
    if (valid[row] == 0) { distances[out_id] = uintBitsToFloat(0x7f800000); return; }
    float dot = 0.0, ln = 0.0, l2 = 0.0;
    for (uint i = 0; i < p.dimension; ++i) {
        // Dimension-major storage makes adjacent invocations read adjacent floats.
        float a = vectors[i * p.rows + row], b = query[i];
        if (p.metric == 2) { float d = a - b; l2 += d * d; }
        else { dot += a * b; if (p.metric == 0) ln += a * a; }
    }
    if (p.metric == 2) distances[out_id] = l2;
    else if (p.metric == 1) distances[out_id] = -dot;
    else distances[out_id] = (ln <= 0.0 || p.query_norm <= 0.0) ? 1.0 : 1.0 - dot * inversesqrt(ln * p.query_norm);
}
)glsl";

struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkDeviceSize size = 0; };
struct CacheKey { const Dataset* dataset; size_t field; bool operator==(const CacheKey& x) const { return dataset == x.dataset && field == x.field; } };
struct KeyHash { size_t operator()(const CacheKey& x) const { return std::hash<const void*>{}(x.dataset) ^ (x.field * 0x9e3779b9U); } };
struct Resident {
    Buffer vectors;
    Buffer valid;
    size_t rows = 0;
    size_t dimension = 0;
    size_t segments = 0;
    bool warmed = false;
};

uint32_t FindMemory(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1U << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

class VulkanVectors {
public:
    static VulkanVectors& Instance() { static VulkanVectors value; return value; }
    ~VulkanVectors() { Shutdown(); }

    bool Preload(const Dataset& dataset, size_t field) {
        std::lock_guard lock(mu_);
        if (!Init() || field >= dataset.Fields().size()) return false;
        size_t rows = 0;
        for (const auto& segment : dataset.Segments()) rows += segment.RowCount();
        const size_t dim = dataset.VectorDimension(field);
        CacheKey key{&dataset, field};
        auto found = cache_.find(key);
        if (found != cache_.end() && found->second.rows == rows &&
            found->second.segments == dataset.Segments().size() && found->second.dimension == dim) return true;
        if (rows == 0 || dim == 0) return false;

        uint64_t budget = 4ULL * 1024ULL * 1024ULL * 1024ULL;
        if (const char* value = std::getenv("MIMICDB_VECTOR_GPU_MAX_BYTES")) {
            const unsigned long long parsed = std::strtoull(value, nullptr, 10);
            if (parsed != 0) budget = parsed;
        }
        const uint64_t required = rows * dim * sizeof(float) + rows * sizeof(uint32_t);
        const uint64_t replaced = found == cache_.end() ? 0 :
            found->second.vectors.size + found->second.valid.size;
        if (required > budget || resident_bytes_ - replaced > budget - required) return false;

        std::vector<float> packed(rows * dim, 0.0F);
        std::vector<uint32_t> valid(rows, 0);
        size_t base = 0;
        for (const auto& segment : dataset.Segments()) {
            const auto& column = segment.Fields()[field];
            for (size_t row = 0; row < segment.RowCount(); ++row) {
                size_t stored = 0;
                const float* value = column.VectorFloat32(row, &stored);
                if (value && stored == dim) {
                    for (size_t d = 0; d < dim; ++d) packed[d * rows + base + row] = value[d];
                    valid[base + row] = 1;
                }
            }
            base += segment.RowCount();
        }
        Resident replacement;
        replacement.rows = rows; replacement.dimension = dim; replacement.segments = dataset.Segments().size();
        if (!UploadDevice(packed.data(), packed.size() * sizeof(float), &replacement.vectors) ||
            !UploadDevice(valid.data(), valid.size() * sizeof(uint32_t), &replacement.valid)) {
            Destroy(replacement.vectors); Destroy(replacement.valid); return false;
        }
        if (found != cache_.end()) { resident_bytes_ -= found->second.vectors.size + found->second.valid.size; Destroy(found->second.vectors); Destroy(found->second.valid); found->second = replacement; }
        else cache_.emplace(key, replacement);
        resident_bytes_ += replacement.vectors.size + replacement.valid.size;
        ++uploads_;
        return true;
    }

    bool Search(const Dataset& dataset, size_t field, const float* query, size_t dimension,
                VectorMetric metric, const std::vector<uint32_t>& candidates,
                std::vector<float>* distances) {
        std::lock_guard lock(mu_);
        if (!Init()) return false;
        const auto it = cache_.find({&dataset, field});
        if (it == cache_.end() || it->second.dimension != dimension) return false;
        const uint32_t count = static_cast<uint32_t>(candidates.empty() ? it->second.rows : candidates.size());
        if (count == 0) { distances->clear(); return true; }
        const uint32_t dummy = 0;
        const void* candidate_data = candidates.empty() ? static_cast<const void*>(&dummy) : candidates.data();
        const size_t candidate_bytes = candidates.empty() ? sizeof(dummy) : candidates.size() * sizeof(uint32_t);
        if (!EnsureHost(query, dimension * sizeof(float), &query_buffer_) ||
            !EnsureHost(candidate_data, candidate_bytes, &candidate_buffer_) ||
            !EnsureHost(nullptr, count * sizeof(float), &output_buffer_)) return false;
        std::array<VkDescriptorBufferInfo, 5> infos{{
            {it->second.vectors.buffer, 0, it->second.vectors.size}, {query_buffer_.buffer, 0, query_buffer_.size},
            {candidate_buffer_.buffer, 0, candidate_buffer_.size}, {it->second.valid.buffer, 0, it->second.valid.size},
            {output_buffer_.buffer, 0, output_buffer_.size}}};
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_, i, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[i], nullptr};
        }
        vkUpdateDescriptorSets(device_, writes.size(), writes.data(), 0, nullptr);
        VkCommandBuffer cmd = Begin();
        if (!cmd) return false;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1, &descriptor_, 0, nullptr);
        float query_norm = 0.0F;
        if (metric == VectorMetric::kCosine)
            for (size_t i = 0; i < dimension; ++i) query_norm += query[i] * query[i];
        struct Params { uint32_t dimension, count, metric, sparse, rows; float query_norm; } params{
            static_cast<uint32_t>(dimension), count, static_cast<uint32_t>(metric),
            candidates.empty() ? 0U : 1U, static_cast<uint32_t>(it->second.rows), query_norm};
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        vkCmdDispatch(cmd, (count + 127) / 128, 1, 1);
        const bool ok = Submit(cmd);
        if (ok) {
            distances->resize(count);
            void* mapped = nullptr;
            vkMapMemory(device_, output_buffer_.memory, 0, count * sizeof(float), 0, &mapped);
            std::memcpy(distances->data(), mapped, count * sizeof(float));
            vkUnmapMemory(device_, output_buffer_.memory);
            ++searches_;
        }
        return ok;
    }

    bool Warmup(const Dataset& dataset, size_t field) {
        size_t dimension = 0;
        {
            std::lock_guard lock(mu_);
            const auto found = cache_.find({&dataset, field});
            if (found == cache_.end()) return false;
            if (found->second.warmed) return true;
            found->second.warmed = true;
            dimension = found->second.dimension;
        }
        std::vector<float> zero_query(dimension, 0.0F);
        std::vector<uint32_t> one_candidate{0};
        std::vector<float> ignored;
        return Search(dataset, field, zero_query.data(), dimension,
                      VectorMetric::kDot, one_candidate, &ignored);
    }

    VectorGpuStats Stats() {
        std::lock_guard lock(mu_); Init();
        return {available_, device_name_.c_str(), resident_bytes_, uploads_, searches_};
    }

    void Release(const Dataset& dataset) {
        std::lock_guard lock(mu_);
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (it->first.dataset == &dataset) {
                resident_bytes_ -= it->second.vectors.size + it->second.valid.size;
                Destroy(it->second.vectors); Destroy(it->second.valid);
                it = cache_.erase(it);
            } else ++it;
        }
    }

private:
    bool Init() {
        if (initialized_) return available_;
        initialized_ = true;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "MimicDB", 1, "MimicDB", 1, VK_API_VERSION_1_1};
        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) return false;
        uint32_t count = 0; vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count); vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        for (auto candidate : devices) {
            uint32_t qcount = 0; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qcount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(qcount); vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qcount, queues.data());
            for (uint32_t q = 0; q < qcount; ++q) if (queues[q].queueFlags & VK_QUEUE_COMPUTE_BIT) { physical_ = candidate; queue_family_ = q; break; }
            if (physical_) break;
        }
        if (!physical_) return false;
        float priority = 1.0F;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, queue_family_, 1, &priority};
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) return false;
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical_, &props); device_name_ = props.deviceName;
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, queue_family_};
        if (vkCreateCommandPool(device_, &pci, nullptr, &pool_) != VK_SUCCESS) return false;
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, static_cast<uint32_t>(bindings.size()), bindings.data()};
        if (vkCreateDescriptorSetLayout(device_, &lci, nullptr, &set_layout_) != VK_SUCCESS) return false;
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &set_layout_, 1, &range};
        if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_) != VK_SUCCESS) return false;
        shaderc::Compiler compiler; shaderc::CompileOptions options;
        auto compiled = compiler.CompileGlslToSpv(kShader, shaderc_compute_shader, "mimicdb_vector.comp", options);
        if (compiled.GetCompilationStatus() != shaderc_compilation_status_success) return false;
        std::vector<uint32_t> spirv(compiled.cbegin(), compiled.cend());
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, spirv.size() * 4, spirv.data()};
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &smci, nullptr, &shader) != VK_SUCCESS) return false;
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage = stage;
        cpci.layout = pipeline_layout_;
        const VkResult pipeline_result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline_);
        vkDestroyShaderModule(device_, shader, nullptr);
        if (pipeline_result != VK_SUCCESS) return false;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 1, &size};
        if (vkCreateDescriptorPool(device_, &dpci, nullptr, &descriptor_pool_) != VK_SUCCESS) return false;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptor_pool_, 1, &set_layout_};
        if (vkAllocateDescriptorSets(device_, &ai, &descriptor_) != VK_SUCCESS) return false;
        available_ = true; return true;
    }

    bool Allocate(VkDeviceSize bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags, Buffer* out) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = std::max<VkDeviceSize>(bytes, 4);
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bci, nullptr, &out->buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device_, out->buffer, &req);
        const uint32_t type = FindMemory(physical_, req.memoryTypeBits, flags);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
        if (vkAllocateMemory(device_, &mai, nullptr, &out->memory) != VK_SUCCESS) return false;
        vkBindBufferMemory(device_, out->buffer, out->memory, 0); out->size = std::max<VkDeviceSize>(bytes, 4); return true;
    }
    bool CreateHost(const void* data, size_t bytes, Buffer* out) {
        if (!Allocate(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, out)) return false;
        if (data) { void* mapped = nullptr; vkMapMemory(device_, out->memory, 0, bytes, 0, &mapped); std::memcpy(mapped, data, bytes); vkUnmapMemory(device_, out->memory); }
        return true;
    }
    bool EnsureHost(const void* data, size_t bytes, Buffer* out) {
        if (out->size < bytes) {
            Destroy(*out);
            if (!CreateHost(nullptr, bytes, out)) return false;
        }
        if (data) {
            void* mapped = nullptr;
            if (vkMapMemory(device_, out->memory, 0, bytes, 0, &mapped) != VK_SUCCESS) return false;
            std::memcpy(mapped, data, bytes);
            vkUnmapMemory(device_, out->memory);
        }
        return true;
    }
    bool UploadDevice(const void* data, size_t bytes, Buffer* out) {
        Buffer staging;
        if (!CreateHost(data, bytes, &staging) || !Allocate(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out)) { Destroy(staging); return false; }
        VkCommandBuffer cmd = Begin(); VkBufferCopy copy{0, 0, bytes}; vkCmdCopyBuffer(cmd, staging.buffer, out->buffer, 1, &copy);
        const bool ok = Submit(cmd); Destroy(staging); return ok;
    }
    VkCommandBuffer Begin() {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cmd = VK_NULL_HANDLE; if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
        if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
        return cmd;
    }
    bool Submit(VkCommandBuffer cmd) {
        if (!cmd || vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd, 0, nullptr};
        const bool ok = vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(queue_) == VK_SUCCESS;
        vkFreeCommandBuffers(device_, pool_, 1, &cmd); return ok;
    }
    void Destroy(Buffer& b) { if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr); if (b.memory) vkFreeMemory(device_, b.memory, nullptr); b = {}; }
    void Shutdown() {
        if (device_) vkDeviceWaitIdle(device_);
        for (auto& entry : cache_) { Destroy(entry.second.vectors); Destroy(entry.second.valid); }
        Destroy(query_buffer_); Destroy(candidate_buffer_); Destroy(output_buffer_);
        if (device_) { if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr); if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr); if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr); if (pool_) vkDestroyCommandPool(device_, pool_, nullptr); vkDestroyDevice(device_, nullptr); }
        if (instance_) vkDestroyInstance(instance_, nullptr);
    }

    std::mutex mu_; bool initialized_ = false, available_ = false; std::string device_name_ = "unavailable";
    VkInstance instance_ = VK_NULL_HANDLE; VkPhysicalDevice physical_ = VK_NULL_HANDLE; VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0; VkQueue queue_ = VK_NULL_HANDLE; VkCommandPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE; VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE; VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE; VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    std::unordered_map<CacheKey, Resident, KeyHash> cache_;
    Buffer query_buffer_, candidate_buffer_, output_buffer_;
    uint64_t resident_bytes_ = 0, uploads_ = 0, searches_ = 0;
};
}  // namespace

bool PreloadVectorField(const Dataset& dataset, size_t field_index) {
    auto& gpu = VulkanVectors::Instance();
    return gpu.Preload(dataset, field_index) && gpu.Warmup(dataset, field_index);
}
void ReleaseVectorGpuDataset(const Dataset& dataset) { VulkanVectors::Instance().Release(dataset); }
VectorGpuStats GetVectorGpuStats() { return VulkanVectors::Instance().Stats(); }

// Used only by vector_search.cpp; kept out of the public API because scores are an implementation detail.
bool VectorGpuScore(const Dataset& dataset, size_t field, const float* query, size_t dimension,
                    VectorMetric metric, const std::vector<uint32_t>& candidates, std::vector<float>* distances) {
    return VulkanVectors::Instance().Search(dataset, field, query, dimension, metric, candidates, distances);
}
}  // namespace mimicdb

#else

namespace mimicdb {
bool PreloadVectorField(const Dataset&, size_t) { return false; }
void ReleaseVectorGpuDataset(const Dataset&) {}
VectorGpuStats GetVectorGpuStats() { return {}; }
bool VectorGpuScore(const Dataset&, size_t, const float*, size_t, VectorMetric,
                    const std::vector<uint32_t>&, std::vector<float>*) { return false; }
}  // namespace mimicdb

#endif
