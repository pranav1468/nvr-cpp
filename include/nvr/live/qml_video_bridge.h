#pragma once

#include "nvr/common/types.h"
#include "nvr/live/display_backend.h"
#include "nvr/live/live_controller.h"
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

    // JSON state serialization for QML view bindings
    std::string GetGridStateJson() const;

    // Callback notification when a new frame is rendered on a tile
    using FrameRenderCallback = std::function<void(int tile_index, int channel_id, uint64_t revision)>;
    void SetFrameRenderCallback(FrameRenderCallback cb);

private:
    QmlVideoBridge() = default;
    ~QmlVideoBridge() override = default;
    QmlVideoBridge(const QmlVideoBridge&) = delete;
    QmlVideoBridge& operator=(const QmlVideoBridge&) = delete;

    struct TileData {
        int channel_id{0};
        uint64_t revision{0};
        DecodedFramePtr frame;
    };

    mutable std::mutex mutex_;
    std::unordered_map<int, TileData> tiles_;
    FrameRenderCallback render_callback_;
    int width_{1920};
    int height_{1080};
};

} // namespace nvr
