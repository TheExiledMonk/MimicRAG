#pragma once

#include "mimicrag/rag_engine.h"
#include <atomic>

namespace mimicrag {
class HttpServer {
public:
    explicit HttpServer(RagEngine& engine) : engine_(engine) {}
    void Run();
private:
    RagEngine& engine_;
};
}  // namespace mimicrag
