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

    void Configure(const ApiConfig& config);
    bool Start();
    void Stop();
    bool IsRunning() const { return is_running_.load(); }
    int GetPort() const { return config_.port; }

private:
    RestServer();
    ~RestServer();
    RestServer(const RestServer&) = delete;
    RestServer& operator=(const RestServer&) = delete;

    void ListenerLoop();
    void HandleClient(int client_fd);
    void HandleStreamClient(int client_fd, int channel_id, StreamType stream_type);
    void HandleSnapshotClient(int client_fd, int channel_id);

    ApiConfig config_;
    std::atomic<bool> is_running_{false};
    int server_fd_{-1};
    std::thread listener_thread_;
    std::unique_ptr<ThreadPool> worker_pool_;
};

} // namespace nvr
