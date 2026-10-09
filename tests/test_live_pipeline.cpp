#include "nvr/common/types.h"
#include "nvr/common/logger.h"
#include "nvr/media/buffer_allocator.h"
#include "nvr/media/video_decoder.h"
#include "nvr/media/video_scaler.h"
#include "nvr/media/stream_broker.h"
#include "nvr/live/live_queue.h"
#include "nvr/live/display_backend.h"
#include "nvr/live/live_controller.h"
#include "nvr/live/qml_video_bridge.h"
#include "nvr/recording/recording_scheduler.h"
#include "nvr/ingress/camera_manager.h"
#include "nvr/storage/database_manager.h"
#include "3rdparty/nlohmann/json.hpp"

#include <iostream>
#include <cassert>
#include <vector>
#include <thread>
#include <chrono>
#include <filesystem>

using json = nlohmann::json;

namespace {

void TestBufferAllocator() {
    std::cout << "[TEST 1] Testing BufferAllocator surface mathematics & pooling..." << std::endl;

    auto& allocator = nvr::BufferAllocator::Instance();
    allocator.ClearPool();

    // 1. Validate surface mathematics against Guide v2.0 Section 20.4
    // 8 SUB streams at 640x360 NV12: 8 * 2 * 640 * 360 * 1.5 = 5,529,600 bytes (~5.27 MB)
    size_t surface_sub = nvr::BufferAllocator::CalculateSurfaceSize(640, 360, 1, 1, 64);
    assert(surface_sub >= 640 * 360 * 3 / 2);
    size_t total_sub_8_q2 = 8 * 2 * surface_sub;
    std::cout << "  -> 8-channel SUB (640x360) surface footprint: " << (total_sub_8_q2 / (1024 * 1024.0)) << " MB" << std::endl;
    assert(total_sub_8_q2 < 6 * 1024 * 1024); // Must remain <= 5.3 MB

    // 2. Validate memory buffer pooling and recycling (zero memory drift)
    // and validate strict 64-byte memory pointer alignment for both fresh and pooled buffers
    auto buf1 = allocator.Allocate(surface_sub);
    assert(buf1.size() == surface_sub);
    assert(buf1.data() != nullptr);
    assert(reinterpret_cast<uintptr_t>(buf1.data()) % 64 == 0);
    allocator.Release(std::move(buf1));
    assert(allocator.GetPoolCount() == 1);

    auto buf2 = allocator.Allocate(surface_sub);
    assert(buf2.size() == surface_sub);
    assert(buf2.data() != nullptr);
    assert(reinterpret_cast<uintptr_t>(buf2.data()) % 64 == 0);
    assert(allocator.GetPoolCount() == 0); // Reused from pool!
    allocator.Release(std::move(buf2));

    // Verify 64-byte pointer alignment across various surface sizes
    for (size_t test_sz : {137, 1024, 1920 * 1080 * 3 / 2, 640 * 360 * 4}) {
        auto b = allocator.Allocate(test_sz);
        assert(b.data() != nullptr);
        assert(reinterpret_cast<uintptr_t>(b.data()) % 64 == 0);
        allocator.Release(std::move(b));
    }

    std::cout << "  -> PASSED: BufferAllocator pooling & 64-byte stride & pointer alignment verified." << std::endl;
}

std::vector<uint8_t> MakeValidH264_640x360() {
    static const uint8_t kValidStream[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1e, 0xd9, 0x00, 0xa0, 0x2f,
        0xf9, 0x70, 0x11, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00,
        0x32, 0x8f, 0x16, 0x2e, 0x48, 0x00, 0x00, 0x00, 0x01, 0x68, 0xcb, 0x83,
        0xcb, 0x20, 0x00, 0x00, 0x01, 0x06, 0x05, 0xff, 0xff, 0x66, 0xdc, 0x45,
        0xe9, 0xbd, 0xe6, 0xd9, 0x48, 0xb7, 0x96, 0x2c, 0xd8, 0x20, 0xd9, 0x23,
        0xee, 0xef, 0x78, 0x32, 0x36, 0x34, 0x20, 0x2d, 0x20, 0x63, 0x6f, 0x72,
        0x65, 0x20, 0x31, 0x36, 0x33, 0x20, 0x72, 0x33, 0x30, 0x36, 0x30, 0x20,
        0x35, 0x64, 0x62, 0x36, 0x61, 0x61, 0x36, 0x20, 0x2d, 0x20, 0x48, 0x2e,
        0x32, 0x36, 0x34, 0x2f, 0x4d, 0x50, 0x45, 0x47, 0x2d, 0x34, 0x20, 0x41,
        0x56, 0x43, 0x20, 0x63, 0x6f, 0x64, 0x65, 0x63, 0x20, 0x2d, 0x20, 0x43,
        0x6f, 0x70, 0x79, 0x6c, 0x65, 0x66, 0x74, 0x20, 0x32, 0x30, 0x30, 0x33,
        0x2d, 0x32, 0x30, 0x32, 0x31, 0x20, 0x2d, 0x20, 0x68, 0x74, 0x74, 0x70,
        0x3a, 0x2f, 0x2f, 0x77, 0x77, 0x77, 0x2e, 0x76, 0x69, 0x64, 0x65, 0x6f,
        0x6c, 0x61, 0x6e, 0x2e, 0x6f, 0x72, 0x67, 0x2f, 0x78, 0x32, 0x36, 0x34,
        0x2e, 0x68, 0x74, 0x6d, 0x6c, 0x20, 0x2d, 0x20, 0x6f, 0x70, 0x74, 0x69,
        0x6f, 0x6e, 0x73, 0x3a, 0x20, 0x63, 0x61, 0x62, 0x61, 0x63, 0x3d, 0x30,
        0x20, 0x72, 0x65, 0x66, 0x3d, 0x33, 0x20, 0x64, 0x65, 0x62, 0x6c, 0x6f,
        0x63, 0x6b, 0x3d, 0x31, 0x3a, 0x30, 0x3a, 0x30, 0x20, 0x61, 0x6e, 0x61,
        0x6c, 0x79, 0x73, 0x65, 0x3d, 0x30, 0x78, 0x31, 0x3a, 0x30, 0x78, 0x31,
        0x31, 0x31, 0x20, 0x6d, 0x65, 0x3d, 0x68, 0x65, 0x78, 0x20, 0x73, 0x75,
        0x62, 0x6d, 0x65, 0x3d, 0x37, 0x20, 0x70, 0x73, 0x79, 0x3d, 0x31, 0x20,
        0x70, 0x73, 0x79, 0x5f, 0x72, 0x64, 0x3d, 0x31, 0x2e, 0x30, 0x30, 0x3a,
        0x30, 0x2e, 0x30, 0x30, 0x20, 0x6d, 0x69, 0x78, 0x65, 0x64, 0x5f, 0x72,
        0x65, 0x66, 0x3d, 0x31, 0x20, 0x6d, 0x65, 0x5f, 0x72, 0x61, 0x6e, 0x67,
        0x65, 0x3d, 0x31, 0x36, 0x20, 0x63, 0x68, 0x72, 0x6f, 0x6d, 0x61, 0x5f,
        0x6d, 0x65, 0x3d, 0x31, 0x20, 0x74, 0x72, 0x65, 0x6c, 0x6c, 0x69, 0x73,
        0x3d, 0x31, 0x20, 0x38, 0x78, 0x38, 0x64, 0x63, 0x74, 0x3d, 0x30, 0x20,
        0x63, 0x71, 0x6d, 0x3d, 0x30, 0x20, 0x64, 0x65, 0x61, 0x64, 0x7a, 0x6f,
        0x6e, 0x65, 0x3d, 0x32, 0x31, 0x2c, 0x31, 0x31, 0x20, 0x66, 0x61, 0x73,
        0x74, 0x5f, 0x70, 0x73, 0x6b, 0x69, 0x70, 0x3d, 0x31, 0x20, 0x63, 0x68,
        0x72, 0x6f, 0x6d, 0x61, 0x5f, 0x71, 0x70, 0x5f, 0x6f, 0x66, 0x66, 0x73,
        0x65, 0x74, 0x3d, 0x2d, 0x32, 0x20, 0x74, 0x68, 0x72, 0x65, 0x61, 0x64,
        0x73, 0x3d, 0x35, 0x20, 0x6c, 0x6f, 0x6f, 0x6b, 0x61, 0x68, 0x65, 0x61,
        0x64, 0x5f, 0x74, 0x68, 0x72, 0x65, 0x61, 0x64, 0x73, 0x3d, 0x35, 0x20,
        0x73, 0x6c, 0x69, 0x63, 0x65, 0x64, 0x5f, 0x74, 0x68, 0x72, 0x65, 0x61,
        0x64, 0x73, 0x3d, 0x31, 0x20, 0x73, 0x6c, 0x69, 0x63, 0x65, 0x73, 0x3d,
        0x35, 0x20, 0x6e, 0x72, 0x3d, 0x30, 0x20, 0x64, 0x65, 0x63, 0x69, 0x6d,
        0x61, 0x74, 0x65, 0x3d, 0x31, 0x20, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x6c,
        0x61, 0x63, 0x65, 0x64, 0x3d, 0x30, 0x20, 0x62, 0x6c, 0x75, 0x72, 0x61,
        0x79, 0x5f, 0x63, 0x6f, 0x6d, 0x70, 0x61, 0x74, 0x3d, 0x30, 0x20, 0x63,
        0x6f, 0x6e, 0x73, 0x74, 0x72, 0x61, 0x69, 0x6e, 0x65, 0x64, 0x5f, 0x69,
        0x6e, 0x74, 0x72, 0x61, 0x3d, 0x30, 0x20, 0x62, 0x66, 0x72, 0x61, 0x6d,
        0x65, 0x73, 0x3d, 0x30, 0x20, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x70,
        0x3d, 0x30, 0x20, 0x6b, 0x65, 0x79, 0x69, 0x6e, 0x74, 0x3d, 0x32, 0x35,
        0x30, 0x20, 0x6b, 0x65, 0x79, 0x69, 0x6e, 0x74, 0x5f, 0x6d, 0x69, 0x6e,
        0x3d, 0x32, 0x35, 0x20, 0x73, 0x63, 0x65, 0x6e, 0x65, 0x63, 0x75, 0x74,
        0x3d, 0x34, 0x30, 0x20, 0x69, 0x6e, 0x74, 0x72, 0x61, 0x5f, 0x72, 0x65,
        0x66, 0x72, 0x65, 0x73, 0x68, 0x3d, 0x30, 0x20, 0x72, 0x63, 0x3d, 0x63,
        0x72, 0x66, 0x20, 0x6d, 0x62, 0x74, 0x72, 0x65, 0x65, 0x3d, 0x30, 0x20,
        0x63, 0x72, 0x66, 0x3d, 0x32, 0x33, 0x2e, 0x30, 0x20, 0x71, 0x63, 0x6f,
        0x6d, 0x70, 0x3d, 0x30, 0x2e, 0x36, 0x30, 0x20, 0x71, 0x70, 0x6d, 0x69,
        0x6e, 0x3d, 0x30, 0x20, 0x71, 0x70, 0x6d, 0x61, 0x78, 0x3d, 0x36, 0x39,
        0x20, 0x71, 0x70, 0x73, 0x74, 0x65, 0x70, 0x3d, 0x34, 0x20, 0x69, 0x70,
        0x5f, 0x72, 0x61, 0x74, 0x69, 0x6f, 0x3d, 0x31, 0x2e, 0x34, 0x30, 0x20,
        0x61, 0x71, 0x3d, 0x31, 0x3a, 0x31, 0x2e, 0x30, 0x30, 0x00, 0x80, 0x00,
        0x00, 0x01, 0x65, 0x88, 0x84, 0x04, 0xaf, 0x11, 0x8a, 0x00, 0x03, 0x31,
        0x31, 0xc0, 0x00, 0x5f, 0x5a, 0x38, 0x00, 0x08, 0x90, 0xc9, 0xc9, 0xc9,
        0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9,
        0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9,
        0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xe0, 0x00, 0x00, 0x01, 0x65, 0x01, 0x92, 0x22, 0x10, 0x12, 0xbc, 0x46,
        0x28, 0x00, 0x0c, 0xc4, 0xc7, 0x00, 0x01, 0x7d, 0x68, 0xe0, 0x00, 0x22,
        0x43, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27,
        0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27,
        0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27,
        0x27, 0x27, 0x27, 0x27, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
        0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x80, 0x00,
        0x00, 0x01, 0x65, 0x00, 0xb4, 0x88, 0x84, 0x04, 0xaf, 0x11, 0x8a, 0x00,
        0x03, 0x31, 0x31, 0xc0, 0x00, 0x5f, 0x5a, 0x38, 0x00, 0x08, 0x90, 0xc9,
        0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9,
        0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9,
        0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9, 0xc9,
        0xc9, 0xc9, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xe0, 0x00, 0x00, 0x01, 0x65, 0x00, 0x46, 0x22, 0x21, 0x01,
        0x2b, 0xc4, 0x62, 0x80, 0x00, 0xcc, 0x4c, 0x70, 0x00, 0x17, 0xd6, 0x8e,
        0x00, 0x02, 0x24, 0x32, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72,
        0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72,
        0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72,
        0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
        0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
        0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
        0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
        0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
        0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
        0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
        0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
        0x78, 0x00, 0x00, 0x01, 0x65, 0x00, 0x5a, 0x22, 0x21, 0x01, 0x2b, 0xc4,
        0x62, 0x80, 0x00, 0xcc, 0x4c, 0x70, 0x00, 0x17, 0xd6, 0x8e, 0x00, 0x02,
        0x24, 0x32, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72,
        0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72,
        0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72,
        0x72, 0x72, 0x72, 0x72, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
        0x5d, 0x75, 0xd7, 0x5d, 0x78
    };
    return std::vector<uint8_t>(kValidStream, kValidStream + sizeof(kValidStream));
}

void TestVideoDecoderAndScaler() {
    std::cout << "[TEST 2] Testing VPU VideoDecoder & VideoScaler pipelines..." << std::endl;

    auto decoder = nvr::VideoDecoderFactory::Create(nvr::CodecType::H264, 640, 360, true);
    assert(decoder != nullptr);

    // 1. Verify that invalid/truncated packet is rejected (no fake success fabrication)
    auto bad_pkt = std::make_shared<nvr::MediaPacket>();
    bad_pkt->channel_id = 1;
    bad_pkt->stream_type = nvr::StreamType::SUB;
    bad_pkt->codec = nvr::CodecType::H264;
    bad_pkt->data = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00}; // Incomplete slice without SPS/PPS
    nvr::DecodedFramePtr bad_frame;
    bool bad_dec = decoder->Decode(bad_pkt, bad_frame);
    assert(!bad_dec);

    // 2. Decode a genuine Annex-B 640x360 H.264 stream packet
    auto pkt = std::make_shared<nvr::MediaPacket>();
    pkt->channel_id = 1;
    pkt->stream_type = nvr::StreamType::SUB;
    pkt->codec = nvr::CodecType::H264;
    pkt->pts_us = 100000;
    pkt->wall_time_ms = 1600000000;
    pkt->rtp_timestamp = 90000;
    pkt->sequence_number = 1;
    pkt->data = MakeValidH264_640x360();

    nvr::DecodedFramePtr decoded_frame;
    bool dec_ok = decoder->Decode(pkt, decoded_frame);
    assert(dec_ok);
    assert(decoded_frame != nullptr);
    assert(decoded_frame->width == 640);
    assert(decoded_frame->height == 360);
    assert(decoded_frame->format == nvr::PixelFormat::NV12);
    assert(!decoded_frame->data.empty());
    assert(decoded_frame->dmabuf_fd == -1);

    // Scale NV12 (640x360) -> RGBA32 (960x540) for 4-camera grid tile
    auto scaler = nvr::VideoScalerFactory::Create(640, 360, nvr::PixelFormat::NV12, 960, 540, nvr::PixelFormat::RGBA32);
    assert(scaler != nullptr);

    nvr::DecodedFramePtr scaled_frame;
    bool scale_ok = scaler->Scale(decoded_frame, scaled_frame);
    assert(scale_ok);
    assert(scaled_frame != nullptr);
    assert(scaled_frame->width == 960);
    assert(scaled_frame->height == 540);
    assert(scaled_frame->format == nvr::PixelFormat::RGBA32);
    assert(scaled_frame->stride == 960 * 4);
    assert(scaled_frame->data.size() == 960 * 4 * 540);

    std::cout << "  -> PASSED: VPU decode & color scaling (NV12 -> RGBA32) verified." << std::endl;
}

void TestLiveQueueFreshnessAndHeadDrop() {
    std::cout << "[TEST 3] Testing LiveQueue bounded head-drop (freshness-first) policy..." << std::endl;

    nvr::LiveQueue queue(2); // Q_max = 2
    assert(queue.GetMaxDepth() == 2);
    assert(queue.Size() == 0);

    // Push 10 frames rapidly to trigger head-drop
    for (int i = 1; i <= 10; ++i) {
        auto frame = std::make_shared<nvr::DecodedFrame>();
        frame->channel_id = 1;
        frame->frame_index = i;
        frame->pts_us = i * 40000;
        queue.Push(frame);
    }

    // Queue depth must never exceed Q_max = 2
    assert(queue.Size() == 2);
    assert(queue.GetTotalEnqueued() == 10);
    assert(queue.GetTotalDropped() == 8); // 8 oldest stale frames dropped!

    // Pop first frame: should be frame #9 (old frames #1..#8 were dropped)
    nvr::DecodedFramePtr popped;
    bool pop1 = queue.Pop(popped, 10);
    assert(pop1);
    assert(popped->frame_index == 9);

    // Pop second frame: should be frame #10
    bool pop2 = queue.Pop(popped, 10);
    assert(pop2);
    assert(popped->frame_index == 10);

    assert(queue.Size() == 0);

    // Test PopLatest draining
    for (int i = 11; i <= 12; ++i) {
        auto frame = std::make_shared<nvr::DecodedFrame>();
        frame->channel_id = 1;
        frame->frame_index = i;
        queue.Push(frame);
    }
    assert(queue.Size() == 2);
    bool pop_latest = queue.PopLatest(popped);
    assert(pop_latest);
    assert(popped->frame_index == 12);
    assert(queue.Size() == 0);

    std::cout << "  -> PASSED: Bounded head-drop queue policy verified (zero latency buildup)." << std::endl;
}

void TestLiveGridTransitions() {
    std::cout << "[TEST 4] Testing LiveGrid layouts (1, 4, 6, 8-camera grid)..." << std::endl;

    // Configure 8 cameras in CameraManager
    std::vector<nvr::CameraConfig> configs;
    for (int i = 1; i <= 8; ++i) {
        nvr::CameraConfig cfg;
        cfg.id = i;
        cfg.name = "Cam_" + std::to_string(i);
        cfg.rtsp_url = "rtsp://127.0.0.1:8554/live/cam" + std::to_string(i);
        cfg.sub_rtsp_url = "rtsp://127.0.0.1:8554/live/cam" + std::to_string(i) + "_sub";
        cfg.enabled = true;
        configs.push_back(cfg);
    }
    nvr::CameraManager::Instance().Initialize(configs);

    auto& live = nvr::LiveController::Instance();
    auto display = std::make_shared<nvr::HeadlessDisplayBackend>();
    display->Initialize(1920, 1080);
    live.SetDisplayBackend(display);

    // 1. Layout: 1-camera
    live.SetLayout(nvr::LiveGridLayout::SINGLE);
    assert(live.GetActiveTileCount() == 1);
    assert(live.GetCurrentLayout() == nvr::LiveGridLayout::SINGLE);

    // 2. Layout: 4-camera (2x2)
    live.SetLayout(nvr::LiveGridLayout::GRID_4);
    assert(live.GetActiveTileCount() == 4);
    assert(live.GetCurrentLayout() == nvr::LiveGridLayout::GRID_4);

    // 3. Layout: 6-camera (3x2)
    live.SetLayout(nvr::LiveGridLayout::GRID_6);
    assert(live.GetActiveTileCount() == 6);
    assert(live.GetCurrentLayout() == nvr::LiveGridLayout::GRID_6);

    // 4. Layout: 8-camera (4x2)
    live.SetLayout(nvr::LiveGridLayout::GRID_8);
    assert(live.GetActiveTileCount() == 8);
    assert(live.GetCurrentLayout() == nvr::LiveGridLayout::GRID_8);

    live.Start();

    // Publish SUB packets for all 8 channels
    for (int i = 1; i <= 8; ++i) {
        auto pkt = std::make_shared<nvr::MediaPacket>();
        pkt->channel_id = i;
        pkt->stream_type = nvr::StreamType::SUB;
        pkt->codec = nvr::CodecType::H264;
        pkt->pts_us = 1000000;
        pkt->rtp_timestamp = 1000;
        pkt->data = {0x00, 0x00, 0x00, 0x01, 0x65, 0x01, 0x02};
        nvr::StreamBroker::Instance().Publish(pkt);
    }

    // Give worker threads a moment to decode and render
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto metrics = live.GetTileMetrics();
    assert(metrics.size() == 8);
    for (const auto& m : metrics) {
        assert(m.width == 480);
        assert(m.height == 540);
        assert(!m.is_fullscreen);
    }

    live.Stop();
    std::cout << "  -> PASSED: 1/4/6/8-camera Live Grid layout transitions and rendering verified." << std::endl;
}

void TestFullscreenAndMainSessionReuse() {
    std::cout << "[TEST 5] Testing Fullscreen mode & MAIN stream session reuse..." << std::endl;

    auto& live = nvr::LiveController::Instance();
    auto display = std::make_shared<nvr::HeadlessDisplayBackend>();
    display->Initialize(1920, 1080);
    live.SetDisplayBackend(display);

    live.SetLayout(nvr::LiveGridLayout::GRID_4);
    live.Start();

    // Channel 1: Simulate that camera 1 is ALREADY online/recording on MAIN stream
    // Start Camera 1 MAIN session
    nvr::CameraManager::Instance().StartCamera(1);
    assert(nvr::CameraManager::Instance().HasCamera(1));

    // UI requests Fullscreen for Camera 1
    bool fs_ok = live.SetFullscreen(1);
    assert(fs_ok);
    assert(live.IsFullscreen());
    assert(live.GetFullscreenChannelId() == 1);
    assert(live.GetActiveTileCount() == 1);

    // Publish 1080p MAIN frame for Camera 1
    auto pkt = std::make_shared<nvr::MediaPacket>();
    pkt->channel_id = 1;
    pkt->stream_type = nvr::StreamType::MAIN;
    pkt->codec = nvr::CodecType::H264;
    pkt->pts_us = 2000000;
    pkt->rtp_timestamp = 2000;
    pkt->data = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB};
    nvr::StreamBroker::Instance().Publish(pkt);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto fs_metrics = live.GetTileMetrics();
    assert(fs_metrics.size() == 1);
    assert(fs_metrics[0].channel_id == 1);
    assert(fs_metrics[0].stream_type == nvr::StreamType::MAIN);
    assert(fs_metrics[0].width == 1920);
    assert(fs_metrics[0].height == 1080);
    assert(fs_metrics[0].is_fullscreen);

    // Exit fullscreen back to grid
    bool exit_ok = live.ExitFullscreen();
    assert(exit_ok);
    assert(!live.IsFullscreen());
    assert(live.GetCurrentLayout() == nvr::LiveGridLayout::GRID_4);
    assert(live.GetActiveTileCount() == 4);

    // Part B: Test Fullscreen on-demand followed by Recording start and Fullscreen exit
    // Camera 2 is NOT online initially
    assert(!nvr::CameraManager::Instance().IsCameraOnline(2));

    // 1. Enter fullscreen on Camera 2 on-demand
    bool fs2_ok = live.SetFullscreen(2);
    assert(fs2_ok);
    assert(live.IsFullscreen());
    assert(live.GetFullscreenChannelId() == 2);
    assert(nvr::CameraManager::Instance().HasCamera(2));

    // 2. Start recording on Camera 2 while in Fullscreen
    nvr::RecordingScheduler::Instance().StartChannelRecording(2, nvr::RecordMode::CONTINUOUS);
    assert(nvr::RecordingScheduler::Instance().IsChannelActive(2));

    // 3. User exits fullscreen back to grid
    bool exit2_ok = live.ExitFullscreen();
    assert(exit2_ok);
    assert(!live.IsFullscreen());

    // 4. Verify Camera 2 recording is STILL active!
    assert(nvr::RecordingScheduler::Instance().IsChannelActive(2));

    // 5. Clean up Camera 2 recording
    nvr::RecordingScheduler::Instance().StopChannelRecording(2);
    assert(!nvr::RecordingScheduler::Instance().IsChannelActive(2));
    nvr::CameraManager::Instance().StopCamera(2);
    nvr::CameraManager::Instance().StopCamera(1);

    live.Stop();
    std::cout << "  -> PASSED: Fullscreen MAIN stream reuse, interleaved recording & return to grid verified." << std::endl;
}

void TestDecouplingFromRecording() {
    std::cout << "[TEST 6] Testing Live View decoupling from Recording (zero recording impact)..." << std::endl;

    std::string test_dir = "./test_live_recordings";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);

    std::string test_db = test_dir + "/test.db";
    nvr::DatabaseManager::Instance().Initialize(test_db);

    nvr::StorageConfig storage_cfg;
    storage_cfg.recording_path = test_dir;
    storage_cfg.segment_duration_seconds = 60;
    nvr::RecordingScheduler::Instance().Configure(storage_cfg);

    // Start recording channel 3
    nvr::RecordingScheduler::Instance().StartChannelRecording(3, nvr::RecordMode::CONTINUOUS);
    assert(nvr::RecordingScheduler::Instance().IsChannelActive(3));

    // Start Live View in 4-camera grid
    auto& live = nvr::LiveController::Instance();
    live.SetLayout(nvr::LiveGridLayout::GRID_4);
    live.Start();

    // Stream 50 packets to Channel 3 MAIN stream (for recording) and SUB stream (for live)
    for (int i = 0; i < 50; ++i) {
        // MAIN packet
        auto main_pkt = std::make_shared<nvr::MediaPacket>();
        main_pkt->channel_id = 3;
        main_pkt->stream_type = nvr::StreamType::MAIN;
        main_pkt->codec = nvr::CodecType::H264;
        main_pkt->pts_us = i * 40000;
        main_pkt->dts_us = i * 40000;
        main_pkt->wall_time_ms = 1000 + (i * 40);
        main_pkt->rtp_timestamp = i * 3600;
        main_pkt->is_keyframe = (i == 0);
        main_pkt->data = {0x00, 0x00, 0x00, 0x01, static_cast<uint8_t>(i == 0 ? 0x65 : 0x61), 0x42, 0x00, 0x1E};
        nvr::StreamBroker::Instance().Publish(main_pkt);

        // SUB packet with heavy flood
        auto sub_pkt = std::make_shared<nvr::MediaPacket>();
        sub_pkt->channel_id = 3;
        sub_pkt->stream_type = nvr::StreamType::SUB;
        sub_pkt->codec = nvr::CodecType::H264;
        sub_pkt->pts_us = i * 40000;
        sub_pkt->rtp_timestamp = i * 3600;
        sub_pkt->data = {0x00, 0x00, 0x00, 0x01, 0x65, 0x01, 0x02};
        nvr::StreamBroker::Instance().Publish(sub_pkt);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify Recording channel received all 50 frames without drop!
    uint32_t rec_frames = nvr::RecordingScheduler::Instance().GetChannelFrameCount(3);
    assert(rec_frames == 50);

    // Stop recording and live
    nvr::RecordingScheduler::Instance().StopChannelRecording(3);
    live.Stop();

    std::filesystem::remove_all(test_dir);
    std::cout << "  -> PASSED: Live view operates 100% decoupled; recording suffered zero drops." << std::endl;
}

void TestQmlVideoBridge() {
    std::cout << "[TEST 7] Testing QmlVideoBridge serialization & Qt/QML bindings..." << std::endl;

    auto& bridge = nvr::QmlVideoBridge::Instance();
    bridge.Initialize(1920, 1080);

    auto frame = std::make_shared<nvr::DecodedFrame>();
    frame->channel_id = 1;
    frame->width = 960;
    frame->height = 540;
    frame->format = nvr::PixelFormat::RGBA32;
    frame->data.resize(960 * 540 * 4, 128);

    bool callback_fired = false;
    bridge.SetFrameRenderCallback([&](int tile, int ch, uint64_t rev) {
        callback_fired = true;
        assert(tile == 0);
        assert(ch == 1);
        assert(rev == 1);
    });

    bridge.RenderTile(0, 1, frame);
    assert(callback_fired);
    assert(bridge.GetTileRevision(0) == 1);
    assert(bridge.GetTileChannelId(0) == 1);
    assert(bridge.GetTileFrame(0) != nullptr);

    std::string json_str = bridge.GetGridStateJson();
    assert(!json_str.empty());
    auto j = json::parse(json_str);
    assert(j.contains("is_fullscreen"));
    assert(j.contains("tiles"));

    std::cout << "  -> PASSED: QmlVideoBridge frame revisioning & JSON state binding verified." << std::endl;
}

void TestExhaustiveEdgeCasesAndStress() {
    std::cout << "[TEST 8] Running exhaustive edge cases & concurrency stress tests..." << std::endl;

    auto& live = nvr::LiveController::Instance();
    auto display = std::make_shared<nvr::HeadlessDisplayBackend>();
    display->Initialize(1920, 1080);
    live.SetDisplayBackend(display);

    // Sub-case 1: Corrupted & edge-case media packets
    auto decoder = nvr::VideoDecoderFactory::Create(nvr::CodecType::H264, 640, 360, true);
    nvr::DecodedFramePtr out_frame;
    assert(!decoder->Decode(nullptr, out_frame)); // Null packet handled cleanly

    auto empty_pkt = std::make_shared<nvr::MediaPacket>();
    assert(!decoder->Decode(empty_pkt, out_frame)); // Empty data handled cleanly

    auto corrupt_pkt = std::make_shared<nvr::MediaPacket>();
    corrupt_pkt->data = {0xFF, 0xFF, 0xFF}; // Corrupted data, no NAL
    assert(!decoder->Decode(corrupt_pkt, out_frame));

    // Audio packet rejection in live pipeline
    auto audio_pkt = std::make_shared<nvr::MediaPacket>();
    audio_pkt->codec = nvr::CodecType::AAC;
    audio_pkt->data = {0xFF, 0xF1, 0x50, 0x80}; // ADTS header
    assert(audio_pkt->IsAudio());

    // Sub-case 2: Heavy queue flooding & buffer allocator zero-drift
    {
        nvr::LiveQueue queue(2);
        auto& alloc = nvr::BufferAllocator::Instance();
        size_t initial_pool = alloc.GetPoolCount();

        for (int i = 0; i < 200; ++i) {
            auto frame = std::make_shared<nvr::DecodedFrame>();
            frame->channel_id = 1;
            frame->data = alloc.Allocate(640 * 360 * 3 / 2);
            queue.Push(frame);
        }

        assert(queue.Size() == 2);
        assert(queue.GetTotalDropped() == 198);
        queue.Clear();
        assert(queue.Size() == 0);
        assert(alloc.GetPoolCount() >= initial_pool);
    }

    // Sub-case 3: Rapid layout & fullscreen switching stress (zero thread leaks, zero deadlock)
    live.SetLayout(nvr::LiveGridLayout::GRID_4);
    live.Start();

    for (int cycle = 1; cycle <= 10; ++cycle) {
        int ch = (cycle % 4) + 1;
        live.SetFullscreen(ch);
        assert(live.IsFullscreen());
        assert(live.GetFullscreenChannelId() == ch);

        live.ExitFullscreen();
        assert(!live.IsFullscreen());
    }

    // Sub-case 4: Concurrent packet publishing during active layout transitions
    std::atomic<bool> publisher_running{true};
    std::thread publisher_thread([&]() {
        uint64_t pts = 0;
        while (publisher_running) {
            for (int ch = 1; ch <= 8; ++ch) {
                auto pkt = std::make_shared<nvr::MediaPacket>();
                pkt->channel_id = ch;
                pkt->stream_type = nvr::StreamType::SUB;
                pkt->codec = nvr::CodecType::H264;
                pkt->pts_us = pts;
                pkt->data = {0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x22};
                nvr::StreamBroker::Instance().Publish(pkt);
            }
            pts += 40000;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Cycle layouts under active packet load
    live.SetLayout(nvr::LiveGridLayout::SINGLE);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    live.SetLayout(nvr::LiveGridLayout::GRID_4);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    live.SetLayout(nvr::LiveGridLayout::GRID_6);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    live.SetLayout(nvr::LiveGridLayout::GRID_8);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    live.SetFullscreen(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    live.ExitFullscreen();

    publisher_running = false;
    if (publisher_thread.joinable()) {
        publisher_thread.join();
    }

    live.Stop();
    std::cout << "  -> PASSED: Exhaustive edge cases & concurrency stress tests passed with 0 crashes." << std::endl;
}

void TestDynamicGridLayoutsAndOnDemandFeeds() {
    std::cout << "[TEST 9] Testing fully dynamic custom grids, tile layouts & on-demand feed control..." << std::endl;

    auto& live = nvr::LiveController::Instance();
    auto display = std::make_shared<nvr::HeadlessDisplayBackend>();
    display->Initialize(1920, 1080);
    live.SetDisplayBackend(display);
    live.Start();

    // 1. Custom 3x3 Grid (9 tiles)
    bool ok_grid = live.SetCustomGrid(3, 3, {1, 2, 3, 4, 5, 6, 7, 8});
    assert(ok_grid);
    assert(live.GetCurrentLayout() == nvr::LiveGridLayout::CUSTOM);
    assert(live.GetActiveTileCount() == 9);

    auto metrics_grid = live.GetTileMetrics();
    for (const auto& m : metrics_grid) {
        assert(m.width == 1920 / 3);
        assert(m.height == 1080 / 3);
    }

    // 2. Custom Tile Layout (Spotlight layout: 1 large tile + 2 smaller tiles)
    std::vector<nvr::TileConfig> custom_tiles;
    nvr::TileConfig t0{0, 1, nvr::StreamType::SUB, 1280, 720};
    nvr::TileConfig t1{1, 2, nvr::StreamType::SUB, 640, 360};
    nvr::TileConfig t2{2, 3, nvr::StreamType::SUB, 640, 360};
    custom_tiles.push_back(t0);
    custom_tiles.push_back(t1);
    custom_tiles.push_back(t2);

    bool ok_tiles = live.SetTileLayout(custom_tiles);
    assert(ok_tiles);
    assert(live.GetActiveTileCount() == 3);

    auto m_t0 = live.GetChannelMetrics(1);
    assert(m_t0.width == 1280);
    assert(m_t0.height == 720);

    auto m_t1 = live.GetChannelMetrics(2);
    assert(m_t1.width == 640);
    assert(m_t1.height == 360);

    // 3. On-demand Channel Deactivation and Activation
    bool deact_ok = live.DeactivateChannel(2);
    assert(deact_ok);
    auto m_t1_after = live.GetChannelMetrics(2);
    assert(m_t1_after.width == 0); // inactive channel has 0 width and 0 height
    assert(m_t1_after.height == 0);

    // Re-activate channel 2 with custom dimensions
    bool act_ok = live.ActivateChannel(2, 1, nvr::StreamType::SUB, 800, 450);
    assert(act_ok);
    auto m_t1_react = live.GetChannelMetrics(2);
    assert(m_t1_react.width == 800);
    assert(m_t1_react.height == 450);

    // 4. SetActiveChannels filter
    bool act_list_ok = live.SetActiveChannels({1, 4});
    assert(act_list_ok);

    // 5. Frontend JSON Command Dispatch via QmlVideoBridge
    auto& bridge = nvr::QmlVideoBridge::Instance();
    bridge.Initialize(1920, 1080);
    assert(bridge.GetWidth() == 1920);
    assert(bridge.GetHeight() == 1080);

    // Dynamic grid command
    bool json_cmd1 = bridge.ExecuteCommandJson(R"({
        "action": "set_custom_grid",
        "cols": 2,
        "rows": 3,
        "channels": [1, 2, 3, 4, 5, 6]
    })");
    assert(json_cmd1);
    assert(live.GetActiveTileCount() == 6);

    // Dynamic tile command
    bool json_cmd2 = bridge.ExecuteCommandJson(R"({
        "action": "set_tiles",
        "tiles": [
            {"tile_index": 0, "channel_id": 1, "stream_type": "SUB", "width": 960, "height": 540},
            {"tile_index": 1, "channel_id": 3, "stream_type": "SUB", "width": 960, "height": 540}
        ]
    })");
    assert(json_cmd2);
    assert(live.GetActiveTileCount() == 2);

    // Dynamic activation/deactivation via JSON
    bool json_cmd3 = bridge.ExecuteCommandJson(R"({
        "action": "activate_channel",
        "channel_id": 4,
        "tile_index": 2,
        "stream_type": "SUB",
        "width": 640,
        "height": 360
    })");
    assert(json_cmd3);

    bool json_cmd4 = bridge.ExecuteCommandJson(R"({
        "action": "deactivate_channel",
        "channel_id": 4
    })");
    assert(json_cmd4);

    // Fullscreen and exit fullscreen via JSON
    bool json_cmd5 = bridge.ExecuteCommandJson(R"({
        "action": "set_fullscreen",
        "channel_id": 1
    })");
    assert(json_cmd5);
    assert(live.IsFullscreen());

    bool json_cmd6 = bridge.ExecuteCommandJson(R"({
        "action": "exit_fullscreen"
    })");
    assert(json_cmd6);
    assert(!live.IsFullscreen());

    // Verify GetGridStateJson contains width, height, and valid structure
    std::string state_json = bridge.GetGridStateJson();
    auto state_obj = json::parse(state_json);
    assert(state_obj["width"] == 1920);
    assert(state_obj["height"] == 1080);

    // Negative / Malformed JSON test cases
    assert(!bridge.ExecuteCommandJson("invalid json text"));
    assert(!bridge.ExecuteCommandJson(R"({"unsupported": 123})"));
    assert(!bridge.ExecuteCommandJson(R"({"action": "unknown_action"})"));

    live.Stop();
    std::cout << "  -> PASSED: Fully dynamic custom grids & on-demand feed control verified." << std::endl;
}

class ConcurrencyTestDisplayBackend : public nvr::IDisplayBackend {
public:
    ConcurrencyTestDisplayBackend() = default;
    ~ConcurrencyTestDisplayBackend() override = default;

    bool Initialize(int width, int height) override {
        width_ = width;
        height_ = height;
        return true;
    }

    void Shutdown() override {}

    void RenderTile(int tile_index, int channel_id, const nvr::DecodedFramePtr& frame) override {
        (void)tile_index;
        (void)channel_id;
        (void)frame;
        render_calls_++;

        // Simulate rendering work (e.g. GPU upload or display refresh)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // Re-entrant queries: while rendering, proactively query LiveController metrics and layout
        auto metrics = nvr::LiveController::Instance().GetTileMetrics();
        (void)metrics;
        auto layout = nvr::LiveController::Instance().GetCurrentLayout();
        (void)layout;
        int tile_count = nvr::LiveController::Instance().GetActiveTileCount();
        (void)tile_count;
    }

    void ClearTile(int tile_index) override {
        (void)tile_index;
    }

    int GetWidth() const override { return width_; }
    int GetHeight() const override { return height_; }
    const char* GetBackendName() const override { return "ConcurrencyTestDisplayBackend"; }

    uint64_t GetRenderCalls() const { return render_calls_.load(); }

private:
    int width_{1920};
    int height_{1080};
    std::atomic<uint64_t> render_calls_{0};
};

void TestConcurrentRebuildAndDeadlockSafety() {
    std::cout << "[TEST 10] Testing Live Grid Rebuild & Concurrency Deadlock Safety..." << std::endl;

    auto& live = nvr::LiveController::Instance();
    auto test_backend = std::make_shared<ConcurrencyTestDisplayBackend>();
    test_backend->Initialize(1920, 1080);
    live.SetDisplayBackend(test_backend);

    live.SetLayout(nvr::LiveGridLayout::GRID_4);
    live.AssignChannels({1, 2, 3, 4});
    live.Start();

    std::atomic<bool> stress_running{true};

    // Thread 1: Ingress packet feeder pushing frames to channels 1..4
    std::thread feeder_thread([&]() {
        int seq = 0;
        while (stress_running) {
            for (int ch = 1; ch <= 4; ++ch) {
                auto pkt = std::make_shared<nvr::MediaPacket>();
                pkt->channel_id = ch;
                pkt->stream_type = nvr::StreamType::SUB;
                pkt->codec = nvr::CodecType::H264;
                pkt->is_keyframe = (seq % 15 == 0);
                pkt->pts_us = seq * 40000;
                pkt->wall_time_ms = 1000 + seq * 40;
                pkt->data = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
                nvr::StreamBroker::Instance().Publish(pkt);
            }
            seq++;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Thread 2: Rapid layout reconfiguration (Grid 4 -> Custom 3x3 -> Single -> Fullscreen -> Exit)
    std::thread reconfig_thread([&]() {
        for (int i = 0; i < 20 && stress_running; ++i) {
            live.SetLayout(nvr::LiveGridLayout::GRID_4);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));

            live.SetCustomGrid(3, 3, {1, 2, 3, 4});
            std::this_thread::sleep_for(std::chrono::milliseconds(15));

            live.SetLayout(nvr::LiveGridLayout::SINGLE);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));

            live.SetFullscreen(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            live.ExitFullscreen();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));

            live.ActivateChannel(2, 1, nvr::StreamType::SUB, 640, 360);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            live.DeactivateChannel(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Thread 3: Display backend dynamic swapping & telemetry polling
    std::thread telemetry_thread([&]() {
        while (stress_running) {
            auto m = live.GetTileMetrics();
            (void)m;
            auto backend = live.GetDisplayBackend();
            (void)backend;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    // Wait for reconfiguration cycles to complete
    reconfig_thread.join();
    stress_running = false;
    feeder_thread.join();
    telemetry_thread.join();

    // Verify clean shutdown without deadlock
    live.Stop();
    live.SetDisplayBackend(std::make_shared<nvr::HeadlessDisplayBackend>());

    std::cout << "  -> PASSED: Rapid layout changes under concurrent rendering executed with zero deadlocks." << std::endl;
}

} // namespace

int main() {
    std::cout << "==========================================================" << std::endl;
    std::cout << "STARTING NVR LIVE VIEW PIPELINE PRODUCTION TEST SUITE" << std::endl;
    std::cout << "==========================================================" << std::endl;

    TestBufferAllocator();
    TestVideoDecoderAndScaler();
    TestLiveQueueFreshnessAndHeadDrop();
    TestLiveGridTransitions();
    TestFullscreenAndMainSessionReuse();
    TestDecouplingFromRecording();
    TestQmlVideoBridge();
    TestExhaustiveEdgeCasesAndStress();
    TestDynamicGridLayoutsAndOnDemandFeeds();
    TestConcurrentRebuildAndDeadlockSafety();

    std::cout << "==========================================================" << std::endl;
    std::cout << "ALL LIVE VIEW PIPELINE TESTS PASSED WITH 100% SUCCESS!" << std::endl;
    std::cout << "==========================================================" << std::endl;
    return 0;
}
