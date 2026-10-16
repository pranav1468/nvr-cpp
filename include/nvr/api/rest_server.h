#pragma once

#include "nvr/common/types.h"
#include <string>
#include <atomic>
#include <thread>
#include <memory>

namespace nvr {

class ThreadPool;

class RestServer {
public:
    static RestServer& Instance();

    bool Start(int port = 8080, const std::string& web_root = "./web");
    void Stop();
    bool IsRunning() const;

private:
    RestServer();
    ~RestServer();
    RestServer(const RestServer&) = delete;
    RestServer& operator=(const RestServer&) = delete;

    void ServerLoop();
    void HandleClient(int client_fd);
    void HandleLiveStream(int client_fd, int channel_id);

    std::string GetMimeType(const std::string& path) const;
    void ServeFile(int client_fd, const std::string& file_path);
    void SendResponse(int client_fd, int status_code, const std::string& status_text, 
                      const std::string& content_type, const std::string& body);

    int port_{8080};
    std::string web_root_{"./web"};
    int server_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    std::unique_ptr<ThreadPool> worker_pool_;
};

} // namespace nvr
