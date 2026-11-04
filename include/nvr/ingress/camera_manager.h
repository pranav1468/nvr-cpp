#pragma once

#include "nvr/common/types.h"
#include "nvr/ingress/stream_session.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>

namespace nvr {

class CameraManager {
public:
    static CameraManager& Instance();

    void Initialize(const std::vector<CameraConfig>& configs);

    bool AddCamera(const CameraConfig& config);
    bool UpdateCamera(const CameraConfig& config);
    bool RemoveCamera(int channel_id);
    bool GetCamera(int channel_id, CameraConfig& out_cfg) const;
    int FindChannelByRtspUrl(const std::string& rtsp_url, int exclude_channel_id = -1) const;
    bool LoadCamerasFromDb();

    bool StartCamera(int channel_id);
    void StopCamera(int channel_id, bool force = false);

    bool StartSubStream(int channel_id);
    void StopSubStream(int channel_id, bool force = false);

    bool EnsureMainStream(int channel_id);

    void StopAll();

    bool IsCameraOnline(int channel_id) const;
    bool IsSubStreamOnline(int channel_id) const;
    bool HasCamera(int channel_id) const;
    std::vector<CameraConfig> GetCameras() const;
    void SetMaxChannels(size_t max_channels) { max_channels_ = max_channels; }
    size_t GetMaxChannels() const { return max_channels_; }
    void SetConfigFilePath(const std::string& path);
    void SaveCamerasToFile();

private:
    CameraManager() = default;
    ~CameraManager();
    CameraManager(const CameraManager&) = delete;
    CameraManager& operator=(const CameraManager&) = delete;


    struct CameraEntry {
        CameraConfig config;
        std::unique_ptr<StreamSession> main_session;
        std::unique_ptr<StreamSession> sub_session;
        std::unique_ptr<std::thread> event_thread;
        std::shared_ptr<std::atomic<bool>> event_running{std::make_shared<std::atomic<bool>>(false)};
    };

    mutable std::mutex mutex_;
    std::unordered_map<int, CameraEntry> cameras_;
    size_t max_channels_{8};
    std::string config_file_path_;
};

} // namespace nvr
