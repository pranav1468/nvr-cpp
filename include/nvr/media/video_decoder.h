#pragma once

#include "nvr/common/types.h"
#include <memory>
#include <string>
#include <vector>

namespace nvr {

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    virtual bool Initialize(CodecType codec, uint32_t width = 0, uint32_t height = 0) = 0;
    virtual bool Decode(const MediaPacketPtr& packet, DecodedFramePtr& out_frame) = 0;
    virtual void Flush() = 0;
    virtual void Close() = 0;

    virtual bool IsHardwareAccelerated() const = 0;
    virtual const char* GetName() const = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;

    using IoctlFn = int(*)(int fd, unsigned long request, void* arg);
    virtual void SetIoctlHook(IoctlFn hook, bool simulate_hardware = false) {
        (void)hook;
        (void)simulate_hardware;
    }
};

using VideoDecoderPtr = std::unique_ptr<IVideoDecoder>;

class VideoDecoderFactory {
public:
    static VideoDecoderPtr Create(CodecType codec, uint32_t width = 0, uint32_t height = 0, bool prefer_vpu = true);
};

} // namespace nvr
