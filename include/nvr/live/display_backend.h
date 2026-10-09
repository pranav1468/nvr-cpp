#pragma once

#include "nvr/common/types.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace nvr {

class IDisplayBackend {
public:
    virtual ~IDisplayBackend() = default;

    virtual bool Initialize(int window_width, int window_height) = 0;
    virtual void RenderTile(int tile_index, int channel_id, const DecodedFramePtr& frame) = 0;
    virtual void ClearTile(int tile_index) = 0;
    virtual void Shutdown() = 0;
    virtual const char* GetBackendName() const = 0;
};

using DisplayBackendPtr = std::shared_ptr<IDisplayBackend>;

// Headless / Memory compositor backend decoupled from physical DRM/KMS
class HeadlessDisplayBackend : public IDisplayBackend {
public:
    HeadlessDisplayBackend() = default;
    ~HeadlessDisplayBackend() override = default;

    bool Initialize(int window_width, int window_height) override {
        width_ = window_width;
        height_ = window_height;
        initialized_ = true;
        return true;
    }

    void RenderTile(int tile_index, int channel_id, const DecodedFramePtr& frame) override {
        std::lock_guard<std::mutex> lock(mutex_);
        tiles_[tile_index] = frame;
        channel_map_[tile_index] = channel_id;
        render_count_++;
    }

    void ClearTile(int tile_index) override {
        std::lock_guard<std::mutex> lock(mutex_);
        tiles_.erase(tile_index);
        channel_map_.erase(tile_index);
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(mutex_);
        tiles_.clear();
        channel_map_.clear();
        initialized_ = false;
    }

    const char* GetBackendName() const override {
        return "DecoupledHeadlessCompositor";
    }

    DecodedFramePtr GetTileFrame(int tile_index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tiles_.find(tile_index);
        return it != tiles_.end() ? it->second : nullptr;
    }

    uint64_t GetRenderCount() const {
        return render_count_.load();
    }

private:
    int width_{1920};
    int height_{1080};
    bool initialized_{false};
    mutable std::mutex mutex_;
    std::unordered_map<int, DecodedFramePtr> tiles_;
    std::unordered_map<int, int> channel_map_;
    std::atomic<uint64_t> render_count_{0};
};

} // namespace nvr
