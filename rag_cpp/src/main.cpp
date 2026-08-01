#include "mimicrag/config.h"
#include "mimicrag/http_server.h"
#include "mimicrag/rag_engine.h"
#include <iostream>

int main(int argc, char** argv) {
    try {
        const std::string config_path = argc > 1 ? argv[1] : "mimicrag.json";
        auto config = mimicrag::Config::Load(config_path);
        mimicrag::RagEngine engine(std::move(config));
        mimicrag::HttpServer server(engine);
        server.Run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mimicrag_server: " << error.what() << '\n';
        return 1;
    }
}
