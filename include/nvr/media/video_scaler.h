#pragma once

#include "nvr/common/types.h"
#include <memory>

namespace nvr {

class IVideoScaler {
public:
    virtual ~IVideoScaler() = default;

    virtual bool Initialize(uint32_t src_w, uint32_t src_h, PixelFormat src_fmt,
                            uint32_t dst_w, uint32_t dst_h, PixelFormat dst_fmt) = 0;
    virtual bool Scale(const DecodedFramePtr& src, DecodedFramePtr& dst) = 0;

    virtual uint32_t GetDstWidth() const = 0;
    virtual uint32_t GetDstHeight() const = 0;
    virtual PixelFormat GetDstFormat() const = 0;
};

using VideoScalerPtr = std::unique_ptr<IVideoScaler>;

class VideoScalerFactory {
public:
    static VideoScalerPtr Create(uint32_t src_w, uint32_t src_h, PixelFormat src_fmt,
                                 uint32_t dst_w, uint32_t dst_h, PixelFormat dst_fmt);
};

} // namespace nvr
