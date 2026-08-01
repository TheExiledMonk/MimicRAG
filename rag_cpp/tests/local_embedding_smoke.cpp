#include "mimicrag/local_embeddings.h"
#include <cmath>
#include <iostream>
#include <string>

float Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    float dot = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < a.size(); ++i) { dot += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
    return dot / std::sqrt(aa * bb);
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 2;
    mimicrag::LocalEmbeddingConfig config;
    config.enabled = true; config.model_path = argv[1];
    config.gpu_layers = argc == 3 && std::string(argv[2]) == "gpu" ? -1 : 0;
    config.threads = 4;
    config.document_prefix = "search_document: "; config.query_prefix = "search_query: ";
    mimicrag::LocalEmbedder embedder(config);
    auto docs = embedder.Embed({"A database supports vector similarity search.", "Marine biology studies whales and coral reefs."});
    auto query = embedder.Embed({"find similar database vectors"}, true).at(0);
    const float database = Cosine(query, docs[0]), ocean = Cosine(query, docs[1]);
    std::cout << "database=" << database << " ocean=" << ocean << '\n';
    return database > ocean ? 0 : 1;
}
