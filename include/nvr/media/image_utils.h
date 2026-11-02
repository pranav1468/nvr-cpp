#pragma once

#include "nvr/common/types.h"
#include <string>

namespace nvr {

// Fast integer fixed-point YUV/RGB to BMP conversion with optional downsampling
bool ConvertFrameToBmp(const DecodedFramePtr& frame, std::string& out_bmp, uint32_t target_width = 320);

// Atomically save a decoded frame as a .bmp file on disk (writes .tmp then renames)
bool SaveFrameToBmp(const DecodedFramePtr& frame, const std::string& file_path, uint32_t target_width = 320);

} // namespace nvr
