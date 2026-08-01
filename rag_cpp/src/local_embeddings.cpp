#include "mimicrag/local_embeddings.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>

#ifdef MIMICRAG_HAVE_LLAMA
#include <llama.h>
#include <ggml-backend.h>
#endif

namespace mimicrag {
struct LocalEmbedder::Impl {
    LocalEmbeddingConfig config;
    bool gpu = false;
#ifdef MIMICRAG_HAVE_LLAMA
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    const llama_vocab* vocab = nullptr;
#endif
};

LocalEmbedder::LocalEmbedder(const LocalEmbeddingConfig& config) : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    if (!config.enabled) return;
#ifndef MIMICRAG_HAVE_LLAMA
    throw std::runtime_error("local embeddings requested but MimicRAG was built without MIMICRAG_ENABLE_LLAMA=ON");
#else
    static const int backend = [] { llama_backend_init(); return 1; }();
    (void) backend;
    auto model_params = llama_model_default_params();
    const bool gpu_available = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU) != nullptr;
    model_params.n_gpu_layers = gpu_available ? config.gpu_layers : 0;
    impl_->model = llama_model_load_from_file(config.model_path.c_str(), model_params);
    if (!impl_->model && model_params.n_gpu_layers != 0) {
        model_params.n_gpu_layers = 0;
        impl_->model = llama_model_load_from_file(config.model_path.c_str(), model_params);
    }
    impl_->gpu = model_params.n_gpu_layers != 0;
    if (!impl_->model) throw std::runtime_error("failed to load local GGUF embedding model: " + config.model_path);
    auto context_params = llama_context_default_params();
    context_params.n_ctx = static_cast<uint32_t>(config.context_size);
    context_params.n_batch = static_cast<uint32_t>(config.context_size);
    context_params.n_ubatch = static_cast<uint32_t>(config.context_size);
    context_params.n_threads = config.threads > 0 ? config.threads : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    context_params.n_threads_batch = context_params.n_threads;
    context_params.embeddings = true;
    context_params.pooling_type = LLAMA_POOLING_TYPE_MEAN;
    impl_->context = llama_init_from_model(impl_->model, context_params);
    if (!impl_->context && impl_->gpu) {
        llama_model_free(impl_->model);
        model_params.n_gpu_layers = 0;
        impl_->model = llama_model_load_from_file(config.model_path.c_str(), model_params);
        impl_->gpu = false;
        if (impl_->model) impl_->context = llama_init_from_model(impl_->model, context_params);
    }
    if (!impl_->context) throw std::runtime_error("failed to create local embedding context");
    impl_->vocab = llama_model_get_vocab(impl_->model);
#endif
}

LocalEmbedder::~LocalEmbedder() {
#ifdef MIMICRAG_HAVE_LLAMA
    if (impl_->context) llama_free(impl_->context);
    if (impl_->model) llama_model_free(impl_->model);
#endif
}

bool LocalEmbedder::Available() const {
#ifdef MIMICRAG_HAVE_LLAMA
    return impl_->context != nullptr;
#else
    return false;
#endif
}
bool LocalEmbedder::UsingGpu() const { return impl_->gpu; }
size_t LocalEmbedder::Dimension() const {
#ifdef MIMICRAG_HAVE_LLAMA
    return impl_->model ? static_cast<size_t>(llama_model_n_embd_out(impl_->model)) : 0;
#else
    return 0;
#endif
}
std::string LocalEmbedder::Identity() const { return "llama.cpp\n" + impl_->config.model_path + "\n" + impl_->config.document_prefix + "\n" + impl_->config.query_prefix; }

std::vector<std::vector<float>> LocalEmbedder::Embed(const std::vector<std::string>& texts, bool query) {
#ifndef MIMICRAG_HAVE_LLAMA
    (void) texts; (void) query;
    throw std::runtime_error("local llama.cpp embeddings are unavailable in this build");
#else
    std::vector<std::vector<float>> output;
    const int32_t dimension = llama_model_n_embd_out(impl_->model);
    for (const auto& original : texts) {
        const std::string text = (query ? impl_->config.query_prefix : impl_->config.document_prefix) + original;
        int32_t count = llama_tokenize(impl_->vocab, text.data(), static_cast<int32_t>(text.size()), nullptr, 0, true, false);
        if (count == INT32_MIN) throw std::runtime_error("local embedding tokenization overflow");
        count = count < 0 ? -count : count;
        std::vector<llama_token> tokens(static_cast<size_t>(count));
        count = llama_tokenize(impl_->vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(), count, true, false);
        if (count <= 0) throw std::runtime_error("local embedding tokenization failed");
        if (count > impl_->config.context_size) throw std::runtime_error("local embedding input exceeds configured context size");
        tokens.resize(static_cast<size_t>(count));
        auto* memory = llama_get_memory(impl_->context);
        if (memory) llama_memory_clear(memory, true);
        auto batch = llama_batch_init(count, 0, 1);
        batch.n_tokens = count;
        for (int32_t i = 0; i < count; ++i) {
            batch.token[i] = tokens[static_cast<size_t>(i)];
            batch.pos[i] = i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = true;
        }
        const int status = llama_model_has_encoder(impl_->model) || !memory
            ? llama_encode(impl_->context, batch)
            : llama_decode(impl_->context, batch);
        if (status != 0) throw std::runtime_error("local embedding inference failed");
        float* values = llama_get_embeddings_seq(impl_->context, 0);
        if (!values) { llama_batch_free(batch); throw std::runtime_error("local model produced no pooled embedding"); }
        std::vector<float> vector(values, values + dimension);
        llama_batch_free(batch);
        float norm = 0.0F;
        for (float value : vector) norm += value * value;
        norm = std::sqrt(norm);
        if (norm > 0.0F) for (float& value : vector) value /= norm;
        output.push_back(std::move(vector));
    }
    return output;
#endif
}
}  // namespace mimicrag
