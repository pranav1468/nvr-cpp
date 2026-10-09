#include "nvr/media/video_scaler.h"
#include "nvr/media/buffer_allocator.h"
#include <algorithm>
#include <cstring>

namespace nvr {

namespace {

inline uint8_t Clamp8(int val) {
    return static_cast<uint8_t>(std::clamp(val, 0, 255));
}

} // namespace

class VideoScalerImpl : public IVideoScaler {
public:
    VideoScalerImpl(uint32_t src_w, uint32_t src_h, PixelFormat src_fmt,
                    uint32_t dst_w, uint32_t dst_h, PixelFormat dst_fmt)
        : src_w_(src_w), src_h_(src_h), src_fmt_(src_fmt),
          dst_w_(dst_w), dst_h_(dst_h), dst_fmt_(dst_fmt) {}

    bool Initialize(uint32_t src_w, uint32_t src_h, PixelFormat src_fmt,
                    uint32_t dst_w, uint32_t dst_h, PixelFormat dst_fmt) override {
        src_w_ = src_w;
        src_h_ = src_h;
        src_fmt_ = src_fmt;
        dst_w_ = dst_w;
        dst_h_ = dst_h;
        dst_fmt_ = dst_fmt;
        return true;
    }

    bool Scale(const DecodedFramePtr& src, DecodedFramePtr& dst) override {
        if (!src || src->data.empty() || src->width == 0 || src->height == 0) {
            return false;
        }

        uint32_t out_w = (dst_w_ > 0) ? dst_w_ : src->width;
        uint32_t out_h = (dst_h_ > 0) ? dst_h_ : src->height;
        PixelFormat out_fmt = (dst_fmt_ != PixelFormat::UNKNOWN) ? dst_fmt_ : src->format;

        dst = std::make_shared<DecodedFrame>();
        dst->channel_id = src->channel_id;
        dst->stream_type = src->stream_type;
        dst->width = out_w;
        dst->height = out_h;
        dst->format = out_fmt;
        dst->pts_us = src->pts_us;
        dst->wall_time_ms = src->wall_time_ms;
        dst->frame_index = src->frame_index;

        if (out_fmt == PixelFormat::RGBA32) {
            dst->stride = out_w * 4;
            size_t req_bytes = dst->stride * out_h;
            dst->data = BufferAllocator::Instance().Allocate(req_bytes);
            if (dst->data.size() < req_bytes) dst->data.resize(req_bytes, 0);

            // Scale & Convert from NV12 to RGBA32
            ScaleNv12ToRgba(src, dst->data.data(), out_w, out_h, dst->stride);
        } else if (out_fmt == PixelFormat::RGB24) {
            dst->stride = out_w * 3;
            size_t req_bytes = dst->stride * out_h;
            dst->data = BufferAllocator::Instance().Allocate(req_bytes);
            if (dst->data.size() < req_bytes) dst->data.resize(req_bytes, 0);

            // Scale & Convert from NV12 to RGB24
            ScaleNv12ToRgb24(src, dst->data.data(), out_w, out_h, dst->stride);
        } else {
            // NV12 to NV12 scaled
            uint32_t stride = (out_w + 63) & ~size_t(63);
            dst->stride = stride;
            size_t req_bytes = BufferAllocator::CalculateSurfaceSize(out_w, out_h, 1, 1, 64);
            dst->data = BufferAllocator::Instance().Allocate(req_bytes);
            if (dst->data.size() < req_bytes) dst->data.resize(req_bytes, 0);

            ScaleNv12ToNv12(src, dst->data.data(), out_w, out_h, stride);
        }

        return true;
    }

    uint32_t GetDstWidth() const override { return dst_w_; }
    uint32_t GetDstHeight() const override { return dst_h_; }
    PixelFormat GetDstFormat() const override { return dst_fmt_; }

private:
    void ScaleNv12ToRgba(const DecodedFramePtr& src, uint8_t* dst, uint32_t dw, uint32_t dh, uint32_t dst_stride) {
        const uint8_t* src_y = src->data.data();
        uint32_t sw = src->width;
        uint32_t sh = src->height;
        uint32_t src_stride = src->stride;
        const uint8_t* src_uv = src_y + (src_stride * sh);

        // Fixed-point scaling mapping
        for (uint32_t dy = 0; dy < dh; ++dy) {
            uint32_t sy = (dy * sh) / dh;
            uint32_t suv_y = sy / 2;
            const uint8_t* line_y = src_y + (sy * src_stride);
            const uint8_t* line_uv = src_uv + (suv_y * src_stride);
            uint8_t* out_row = dst + (dy * dst_stride);

            for (uint32_t dx = 0; dx < dw; ++dx) {
                uint32_t sx = (dx * sw) / dw;
                uint32_t suv_x = (sx / 2) * 2;

                int y = line_y[sx];
                int u = line_uv[suv_x];
                int v = line_uv[suv_x + 1];

                int c = y - 16;
                int d = u - 128;
                int e = v - 128;

                int r = (298 * c + 409 * e + 128) >> 8;
                int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                int b = (298 * c + 516 * d + 128) >> 8;

                uint8_t* pixel = out_row + (dx * 4);
                pixel[0] = Clamp8(r);
                pixel[1] = Clamp8(g);
                pixel[2] = Clamp8(b);
                pixel[3] = 255; // Alpha
            }
        }
    }

    void ScaleNv12ToRgb24(const DecodedFramePtr& src, uint8_t* dst, uint32_t dw, uint32_t dh, uint32_t dst_stride) {
        const uint8_t* src_y = src->data.data();
        uint32_t sw = src->width;
        uint32_t sh = src->height;
        uint32_t src_stride = src->stride;
        const uint8_t* src_uv = src_y + (src_stride * sh);

        for (uint32_t dy = 0; dy < dh; ++dy) {
            uint32_t sy = (dy * sh) / dh;
            uint32_t suv_y = sy / 2;
            const uint8_t* line_y = src_y + (sy * src_stride);
            const uint8_t* line_uv = src_uv + (suv_y * src_stride);
            uint8_t* out_row = dst + (dy * dst_stride);

            for (uint32_t dx = 0; dx < dw; ++dx) {
                uint32_t sx = (dx * sw) / dw;
                uint32_t suv_x = (sx / 2) * 2;

                int y = line_y[sx];
                int u = line_uv[suv_x];
                int v = line_uv[suv_x + 1];

                int c = y - 16;
                int d = u - 128;
                int e = v - 128;

                int r = (298 * c + 409 * e + 128) >> 8;
                int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                int b = (298 * c + 516 * d + 128) >> 8;

                uint8_t* pixel = out_row + (dx * 3);
                pixel[0] = Clamp8(r);
                pixel[1] = Clamp8(g);
                pixel[2] = Clamp8(b);
            }
        }
    }

    void ScaleNv12ToNv12(const DecodedFramePtr& src, uint8_t* dst, uint32_t dw, uint32_t dh, uint32_t dst_stride) {
        const uint8_t* src_y = src->data.data();
        uint32_t sw = src->width;
        uint32_t sh = src->height;
        uint32_t src_stride = src->stride;
        const uint8_t* src_uv = src_y + (src_stride * sh);
        uint8_t* dst_uv = dst + (dst_stride * dh);

        // Scale Y plane
        for (uint32_t dy = 0; dy < dh; ++dy) {
            uint32_t sy = (dy * sh) / dh;
            const uint8_t* line_y = src_y + (sy * src_stride);
            uint8_t* out_row_y = dst + (dy * dst_stride);
            for (uint32_t dx = 0; dx < dw; ++dx) {
                uint32_t sx = (dx * sw) / dw;
                out_row_y[dx] = line_y[sx];
            }
        }

        // Scale UV plane
        for (uint32_t dy = 0; dy < dh / 2; ++dy) {
            uint32_t sy = (dy * (sh / 2)) / (dh / 2);
            const uint8_t* line_uv = src_uv + (sy * src_stride);
            uint8_t* out_row_uv = dst_uv + (dy * dst_stride);
            for (uint32_t dx = 0; dx < dw / 2; ++dx) {
                uint32_t sx = (dx * (sw / 2)) / (dw / 2);
                out_row_uv[dx * 2] = line_uv[sx * 2];
                out_row_uv[dx * 2 + 1] = line_uv[sx * 2 + 1];
            }
        }
    }

    uint32_t src_w_{0};
    uint32_t src_h_{0};
    PixelFormat src_fmt_{PixelFormat::NV12};
    uint32_t dst_w_{0};
    uint32_t dst_h_{0};
    PixelFormat dst_fmt_{PixelFormat::RGBA32};
};

VideoScalerPtr VideoScalerFactory::Create(uint32_t src_w, uint32_t src_h, PixelFormat src_fmt,
                                         uint32_t dst_w, uint32_t dst_h, PixelFormat dst_fmt) {
    auto scaler = std::make_unique<VideoScalerImpl>(src_w, src_h, src_fmt, dst_w, dst_h, dst_fmt);
    scaler->Initialize(src_w, src_h, src_fmt, dst_w, dst_h, dst_fmt);
    return scaler;
}

} // namespace nvr
