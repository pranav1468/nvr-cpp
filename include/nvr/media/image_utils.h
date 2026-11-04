#pragma once

#include "nvr/common/types.h"
#include <string>

namespace nvr {

// Fast lightweight JPEG conversion (quality: 1-100, default 80, target_width = 0 keeps native resolution)
bool ConvertFrameToJpeg(const DecodedFramePtr& frame, std::string& out_jpeg, int quality = 80, uint32_t target_width = 0);

// Atomically save a decoded frame as a .jpg file on disk (writes .tmp then renames)
bool SaveFrameToJpeg(const DecodedFramePtr& frame, const std::string& file_path, int quality = 80, uint32_t target_width = 0);

// Fast integer fixed-point YUV/RGB to BMP conversion with optional downsampling
bool ConvertFrameToBmp(const DecodedFramePtr& frame, std::string& out_bmp, uint32_t target_width = 320);

// Atomically save a decoded frame as a .bmp file on disk (writes .tmp then renames)
bool SaveFrameToBmp(const DecodedFramePtr& frame, const std::string& file_path, uint32_t target_width = 320);

} // namespace nvr
