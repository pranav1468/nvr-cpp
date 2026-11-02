#pragma once

#include "nvr/common/types.h"
#include "3rdparty/nlohmann/json.hpp"
#include <string>
#include <atomic>
#include <thread>
#include <memory>
#include <mutex>
#include <unordered_map>

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
    const std::string& GetBindAddress() const { return config_.bind_address; }
    bool IsHttpsEnforced() const { return config_.enforce_https; }
    bool IsHstsEnabled() const { return config_.enable_hsts || config_.enforce_https; }
    const ApiConfig& GetConfig() const { return config_; }

    bool IsOriginAllowed(const std::string& origin) const;
    std::string BuildCorsHeaders(const std::string& request_origin, bool is_preflight = false) const;

private:
    RestServer();
    ~RestServer();
    RestServer(const RestServer&) = delete;
    RestServer& operator=(const RestServer&) = delete;

    void ListenerLoop();
    void HandleClient(int client_fd);
    void HandleStreamClient(int client_fd, int channel_id, StreamType stream_type, const std::string& request_origin = "");
    void HandleSnapshotClient(int client_fd, int channel_id);
    void HandleRecordingsList(int client_fd, const nlohmann::json& query_params, const std::string& request_origin);
    void HandleRecordingsDates(int client_fd, const std::string& request_origin);
    void HandleRecordingVideo(int client_fd, const nlohmann::json& query_params, const std::unordered_map<std::string, std::string>& headers, const std::string& request_origin);
    void HandleRecordingLock(int client_fd, const std::string& method, const std::string& path, const std::string& body, const std::string& request_origin);
    void HandleRecordingExport(int client_fd, const std::string& method, const std::string& path, const std::string& body, const std::string& request_origin);
    void HandleRecordingExportStatus(int client_fd, const nlohmann::json& query_params, const std::string& request_origin);
    void HandleRecordingExportDownload(int client_fd, const nlohmann::json& query_params, const std::unordered_map<std::string, std::string>& headers, const std::string& request_origin);

    // WebSocket Low-Latency Streaming (RFC 6455)
    bool HandleWebSocketHandshake(int client_fd, const std::string& request_headers, const std::string& subprotocol = "");
    std::vector<uint8_t> BuildWebSocketFrame(const uint8_t* payload, size_t len, uint8_t opcode = 0x02);
    void HandleWebSocketStream(int client_fd, int channel_id, StreamType stream_type);

    struct SnapshotCacheEntry {
        int64_t pts_us{-1};
        uint32_t width{0};
        uint32_t height{0};
        std::string bmp_bytes;
    };

    ApiConfig config_;
    std::atomic<bool> is_running_{false};
    int server_fd_{-1};
    std::thread listener_thread_;
    std::unique_ptr<ThreadPool> worker_pool_;
    std::unique_ptr<ThreadPool> media_pool_;
    static constexpr int kMaxControlConnections = 128;
    static constexpr int kMaxMediaStreams = 32;

    std::atomic<int> active_connections_{0};
    std::atomic<int> active_media_streams_{0};

    std::mutex snapshot_cache_mutex_;
    std::unordered_map<int, SnapshotCacheEntry> snapshot_cache_;
};

} // namespace nvr
