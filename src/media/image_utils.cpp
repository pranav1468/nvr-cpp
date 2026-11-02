#include "nvr/media/image_utils.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace nvr {

bool ConvertFrameToBmp(const DecodedFramePtr& frame, std::string& out_bmp, uint32_t target_width) {
    if (!frame || frame->data.empty() || frame->width == 0 || frame->height == 0) {
        return false;
    }

    uint32_t w = frame->width;
    uint32_t h = frame->height;
    uint32_t scale = 1;
    if (target_width > 0 && w > target_width) {
        scale = (w + target_width - 1) / target_width;
        if (scale < 1) scale = 1;
    }
    uint32_t out_w = w / scale;
    uint32_t out_h = h / scale;
    if (out_w == 0 || out_h == 0) return false;

    uint32_t row_stride = ((out_w * 3 + 3) / 4) * 4;
    uint32_t image_size = row_stride * out_h;
    uint32_t file_size = 54 + image_size;

    out_bmp.resize(file_size);
    uint8_t* p = reinterpret_cast<uint8_t*>(&out_bmp[0]);

    // BMP Header (14 bytes)
    p[0] = 'B';
    p[1] = 'M';
    *reinterpret_cast<uint32_t*>(p + 2) = file_size;
    *reinterpret_cast<uint32_t*>(p + 6) = 0; // reserved
    *reinterpret_cast<uint32_t*>(p + 10) = 54; // offset to pixel data

    // DIB Header (40 bytes - BITMAPINFOHEADER)
    *reinterpret_cast<uint32_t*>(p + 14) = 40;
    *reinterpret_cast<int32_t*>(p + 18) = static_cast<int32_t>(out_w);
    *reinterpret_cast<int32_t*>(p + 22) = static_cast<int32_t>(out_h); // positive = bottom-up
    *reinterpret_cast<uint16_t*>(p + 26) = 1; // planes
    *reinterpret_cast<uint16_t*>(p + 28) = 24; // 24 bpp (BGR)
    *reinterpret_cast<uint32_t*>(p + 30) = 0; // BI_RGB (uncompressed)
    *reinterpret_cast<uint32_t*>(p + 34) = image_size;
    *reinterpret_cast<int32_t*>(p + 38) = 2835; // 72 DPI
    *reinterpret_cast<int32_t*>(p + 42) = 2835;
    *reinterpret_cast<uint32_t*>(p + 46) = 0;
    *reinterpret_cast<uint32_t*>(p + 50) = 0;

    uint8_t* img_data = p + 54;
    if (frame->format == PixelFormat::NV12) {
        const uint8_t* y_plane = frame->data.data();
        uint32_t stride = (frame->stride > 0) ? frame->stride : w;
        const uint8_t* uv_plane = frame->data.data() + (stride * h);

        for (int r = static_cast<int>(out_h) - 1; r >= 0; --r) {
            uint8_t* row_out = img_data + (static_cast<size_t>(out_h - 1 - r) * row_stride);
            uint32_t src_r = static_cast<uint32_t>(r) * scale;
            const uint8_t* y_row = y_plane + (src_r * stride);
            const uint8_t* uv_row = uv_plane + ((src_r / 2) * stride);
            for (uint32_t c = 0; c < out_w; ++c) {
                uint32_t src_c = c * scale;
                int y = y_row[src_c];
                int u = uv_row[(src_c & ~1)] - 128;
                int v = uv_row[(src_c & ~1) + 1] - 128;

                // 16-bit integer fixed-point color conversion (0 FPU ops)
                row_out[c * 3 + 0] = static_cast<uint8_t>(std::clamp(y + ((454 * u) >> 8), 0, 255));
                row_out[c * 3 + 1] = static_cast<uint8_t>(std::clamp(y - ((88 * u + 183 * v) >> 8), 0, 255));
                row_out[c * 3 + 2] = static_cast<uint8_t>(std::clamp(y + ((359 * v) >> 8), 0, 255));
            }
            for (uint32_t pad = out_w * 3; pad < row_stride; ++pad) {
                row_out[pad] = 0;
            }
        }
    } else if (frame->format == PixelFormat::YUV420P) {
        const uint8_t* y_plane = frame->data.data();
        uint32_t y_stride = (frame->stride > 0) ? frame->stride : w;
        uint32_t uv_stride = y_stride / 2;
        const uint8_t* u_plane = y_plane + (y_stride * h);
        const uint8_t* v_plane = u_plane + (uv_stride * (h / 2));

        for (int r = static_cast<int>(out_h) - 1; r >= 0; --r) {
            uint8_t* row_out = img_data + (static_cast<size_t>(out_h - 1 - r) * row_stride);
            uint32_t src_r = static_cast<uint32_t>(r) * scale;
            const uint8_t* y_row = y_plane + (src_r * y_stride);
            const uint8_t* u_row = u_plane + ((src_r / 2) * uv_stride);
            const uint8_t* v_row = v_plane + ((src_r / 2) * uv_stride);
            for (uint32_t c = 0; c < out_w; ++c) {
                uint32_t src_c = c * scale;
                int y = y_row[src_c];
                int u = u_row[src_c / 2] - 128;
                int v = v_row[src_c / 2] - 128;

                row_out[c * 3 + 0] = static_cast<uint8_t>(std::clamp(y + ((454 * u) >> 8), 0, 255));
                row_out[c * 3 + 1] = static_cast<uint8_t>(std::clamp(y - ((88 * u + 183 * v) >> 8), 0, 255));
                row_out[c * 3 + 2] = static_cast<uint8_t>(std::clamp(y + ((359 * v) >> 8), 0, 255));
            }
            for (uint32_t pad = out_w * 3; pad < row_stride; ++pad) {
                row_out[pad] = 0;
            }
        }
    } else if (frame->format == PixelFormat::RGB24) {
        uint32_t stride = (frame->stride > 0) ? frame->stride : (w * 3);
        for (int r = static_cast<int>(out_h) - 1; r >= 0; --r) {
            uint8_t* row_out = img_data + (static_cast<size_t>(out_h - 1 - r) * row_stride);
            uint32_t src_r = static_cast<uint32_t>(r) * scale;
            const uint8_t* src_row = frame->data.data() + (src_r * stride);
            for (uint32_t c = 0; c < out_w; ++c) {
                uint32_t src_c = c * scale;
                row_out[c * 3 + 0] = src_row[src_c * 3 + 2]; // B
                row_out[c * 3 + 1] = src_row[src_c * 3 + 1]; // G
                row_out[c * 3 + 2] = src_row[src_c * 3 + 0]; // R
            }
            for (uint32_t pad = out_w * 3; pad < row_stride; ++pad) {
                row_out[pad] = 0;
            }
        }
    } else if (frame->format == PixelFormat::RGBA32) {
        uint32_t stride = (frame->stride > 0) ? frame->stride : (w * 4);
        for (int r = static_cast<int>(out_h) - 1; r >= 0; --r) {
            uint8_t* row_out = img_data + (static_cast<size_t>(out_h - 1 - r) * row_stride);
            uint32_t src_r = static_cast<uint32_t>(r) * scale;
            const uint8_t* src_row = frame->data.data() + (src_r * stride);
            for (uint32_t c = 0; c < out_w; ++c) {
                uint32_t src_c = c * scale;
                row_out[c * 3 + 0] = src_row[src_c * 4 + 2]; // B
                row_out[c * 3 + 1] = src_row[src_c * 4 + 1]; // G
                row_out[c * 3 + 2] = src_row[src_c * 4 + 0]; // R
            }
            for (uint32_t pad = out_w * 3; pad < row_stride; ++pad) {
                row_out[pad] = 0;
            }
        }
    } else {
        return false;
    }
    return true;
}

bool SaveFrameToBmp(const DecodedFramePtr& frame, const std::string& file_path, uint32_t target_width) {
    std::string bmp;
    if (!ConvertFrameToBmp(frame, bmp, target_width)) {
        return false;
    }
    std::string tmp_path = file_path + ".tmp";
    std::ofstream ofs(tmp_path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(bmp.data(), bmp.size());
    ofs.close();
    std::error_code ec;
    std::filesystem::rename(tmp_path, file_path, ec);
    return !ec;
}

} // namespace nvr
