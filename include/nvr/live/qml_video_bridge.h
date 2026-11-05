#pragma once

#include "nvr/common/types.h"
#include "nvr/live/display_backend.h"
#include "nvr/live/live_controller.h"
#include "nvr/dispatcher/commands.h"
#include <string>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <atomic>

namespace nvr {

class QmlVideoBridge : public IDisplayBackend {
public:
    static QmlVideoBridge& Instance();

    bool Initialize(int window_width, int window_height) override;
    void RenderTile(int tile_index, int channel_id, const DecodedFramePtr& frame) override;
    void ClearTile(int tile_index) override;
    void Shutdown() override;
    const char* GetBackendName() const override;
    int GetWidth() const override { return width_; }
    int GetHeight() const override { return height_; }

    // Frame retrieval for QML VideoTile rendering
    DecodedFramePtr GetTileFrame(int tile_index) const;
    uint64_t GetTileRevision(int tile_index) const;
    int GetTileChannelId(int tile_index) const;

    // UI Command interface
    void RequestLayout(int layout_count);
    void RequestFullscreen(int channel_id);
    void RequestExitFullscreen();
    bool ExecuteCommandJson(const std::string& command_json);

    // Local UI session management
    void SetSessionToken(const std::string& token);
    std::string GetSessionToken() const;
    void SetLocalUserRole(UserRole role);
    UserRole GetLocalUserRole() const;

    // JSON state serialization for QML view bindings
    std::string GetGridStateJson() const;

    // Callback notification when a new frame is rendered on a tile
    using FrameRenderCallback = std::function<void(int tile_index, int channel_id, uint64_t revision)>;
    void SetFrameRenderCallback(FrameRenderCallback cb);

    // --- NXP Hardware G2D + qml6glsink Zero-Copy Pipeline ---

    // Telemetry metrics for hardware accelerated G2D + qml6glsink pipeline
    struct G2dPipelineMetrics {
        int tile_index{-1};
        int channel_id{-1};
        uint64_t frames_submitted{0};
        uint64_t frames_rendered{0};
        uint64_t frames_dropped{0};
        bool is_active{false};
        bool hardware_accelerated{false};
        std::string pipeline_description;
    };

    // Builds the exact NXP G2D -> glupload -> qml6glsink pipeline description string
    static std::string BuildG2dPipelineDescription(int src_width, int src_height,
                                                  int dst_width, int dst_height,
                                                  const std::string& pixel_format = "NV12");

    // Dynamic hardware capabilities inspection
    bool IsG2dHardwareSupported() const;
    bool IsQml6GlSinkSupported() const;
    bool IsDmabufSupported() const;
    bool IsHardwareAccelerationEnabled() const;
    void SetHardwareAccelerationEnabled(bool enabled);

    // QML surface binding & pipeline lifecycle
    bool AttachTileVideoSurface(int tile_index, void* qquick_surface_item);
    void DetachTileVideoSurface(int tile_index);
    bool IsTileHardwareAccelerated(int tile_index) const;
    G2dPipelineMetrics GetTileHardwareMetrics(int tile_index) const;

private:
    QmlVideoBridge();
    ~QmlVideoBridge() override;
    QmlVideoBridge(const QmlVideoBridge&) = delete;
    QmlVideoBridge& operator=(const QmlVideoBridge&) = delete;

    struct TileData {
        int channel_id{0};
        uint64_t revision{0};
        DecodedFramePtr frame;
    };

    struct ImxG2dTilePipeline {
        int tile_index{-1};
        int channel_id{-1};
        int src_width{0};
        int src_height{0};
        int dst_width{0};
        int dst_height{0};
        std::string pixel_format{"NV12"};
        void* qquick_surface{nullptr};
        void* gst_pipeline{nullptr};
        void* gst_appsrc{nullptr};
        void* gst_sink{nullptr};
        std::string description;
        uint64_t frames_submitted{0};
        uint64_t frames_rendered{0};
        uint64_t frames_dropped{0};
        bool is_active{false};
        bool is_hw_accelerated{false};
    };

    void InitGstRuntime();
    void CleanupTilePipeline(ImxG2dTilePipeline& p);
    bool StartTilePipeline(ImxG2dTilePipeline& p, int src_w, int src_h, int dst_w, int dst_h);
    bool PushFrameToTilePipeline(int tile_index, const DecodedFramePtr& frame);

    // Per-tile NV12 -> BGRx hardware converter (appsrc ! imxvideoconvert_g2d ! appsink).
    // Used when G2D is present but qml6glsink is not: colour conversion runs on the G2D
    // engine at native resolution and the result is handed to Qt without any CPU copy.
    struct G2dConverter;
    DecodedFramePtr ConvertFrameWithG2d(int tile_index, const DecodedFramePtr& frame);
    bool IsG2dAppsinkModeAvailable() const;

    mutable std::mutex mutex_;
    std::unordered_map<int, TileData> tiles_;
    std::unordered_map<int, ImxG2dTilePipeline> hw_pipelines_;
    std::unordered_map<int, std::shared_ptr<G2dConverter>> g2d_converters_;
    FrameRenderCallback render_callback_;
    int width_{1920};
    int height_{1080};

    bool hw_acceleration_enabled_{true};
    bool gst_initialized_{false};
    bool g2d_supported_{false};
    bool g2d_gst_element_{false};
    bool qml6glsink_supported_{false};
    bool dmabuf_supported_{false};
    mutable std::string session_token_;
    mutable UserRole local_role_{UserRole::OPERATOR};
};

} // namespace nvr
