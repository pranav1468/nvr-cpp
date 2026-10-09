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
};

using VideoDecoderPtr = std::unique_ptr<IVideoDecoder>;

class VideoDecoderFactory {
public:
    static VideoDecoderPtr Create(CodecType codec, uint32_t width = 0, uint32_t height = 0, bool prefer_vpu = true);
};

} // namespace nvr
