#pragma once

#include "nvr/common/types.h"
#include "nvr/ingress/stream_session.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>

namespace nvr {

class CameraManager {
public:
    static CameraManager& Instance();

    void Initialize(const std::vector<CameraConfig>& configs);

    bool AddCamera(const CameraConfig& config);
    bool RemoveCamera(int channel_id);

    bool StartCamera(int channel_id);
    void StopCamera(int channel_id);

    void StartAll();
    void StopAll();

    bool IsCameraOnline(int channel_id) const;
    std::vector<CameraConfig> GetCameras() const;

private:
    CameraManager() = default;
    ~CameraManager();
    CameraManager(const CameraManager&) = delete;
    CameraManager& operator=(const CameraManager&) = delete;

    struct CameraEntry {
        CameraConfig config;
        std::unique_ptr<StreamSession> main_session;
    };

    mutable std::mutex mutex_;
    std::unordered_map<int, CameraEntry> cameras_;
};

} // namespace nvr
