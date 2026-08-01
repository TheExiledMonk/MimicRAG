#include "mimicrag/provider.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <string>
#include <thread>

int main() {
    constexpr int port = 18083;
    std::thread upstream([] {
        int server = socket(AF_INET, SOCK_STREAM, 0); int reuse = 1; setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(server, 1)) return;
        int client = accept(server, nullptr, nullptr); char request[4096]; recv(client, request, sizeof(request), 0);
        const std::string body = "data: {\"choices\":[{\"delta\":{\"content\":\"native \"}}]}\n\ndata: {\"choices\":[{\"delta\":{\"content\":\"stream\"}}]}\n\ndata: [DONE]\n\n";
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        send(client, response.data(), response.size(), MSG_NOSIGNAL); close(client); close(server);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    mimicrag::ModelConfig config; config.provider = "openai_compatible"; config.model = "test"; config.base_url = "http://127.0.0.1:" + std::to_string(port); config.timeout_seconds = 2;
    mimicrag::RemoteProvider provider(config); std::string streamed;
    const auto result = provider.Chat(nlohmann::json::array({{{"role", "user"}, {"content", "test"}}}), {}, [&](const std::string& token) { streamed += token; });
    upstream.join(); return result == "native stream" && streamed == result ? 0 : 1;
}
