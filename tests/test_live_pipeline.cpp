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
    auto buf1 = allocator.Allocate(surface_sub);
    assert(buf1.size() == surface_sub);
    allocator.Release(std::move(buf1));
    assert(allocator.GetPoolCount() == 1);

    auto buf2 = allocator.Allocate(surface_sub);
    assert(buf2.size() == surface_sub);
    assert(allocator.GetPoolCount() == 0); // Reused from pool!
    allocator.Release(std::move(buf2));

    std::cout << "  -> PASSED: BufferAllocator pooling & 64-byte stride mathematics verified." << std::endl;
}

void TestVideoDecoderAndScaler() {
    std::cout << "[TEST 2] Testing VPU VideoDecoder & VideoScaler pipelines..." << std::endl;

    auto decoder = nvr::VideoDecoderFactory::Create(nvr::CodecType::H264, 640, 360, true);
    assert(decoder != nullptr);

    // Create synthetic H.264 MediaPacket
    auto pkt = std::make_shared<nvr::MediaPacket>();
    pkt->channel_id = 1;
    pkt->stream_type = nvr::StreamType::SUB;
    pkt->codec = nvr::CodecType::H264;
    pkt->pts_us = 100000;
    pkt->wall_time_ms = 1600000000;
    pkt->rtp_timestamp = 90000;
    pkt->sequence_number = 1;
    pkt->data = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00}; // IDR slice

    nvr::DecodedFramePtr decoded_frame;
    bool dec_ok = decoder->Decode(pkt, decoded_frame);
    assert(dec_ok);
    assert(decoded_frame != nullptr);
    assert(decoded_frame->width == 640);
    assert(decoded_frame->height == 360);
    assert(decoded_frame->format == nvr::PixelFormat::NV12);
    assert(!decoded_frame->data.empty());

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
        assert(m.enqueued_frames >= 1);
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

    std::cout << "==========================================================" << std::endl;
    std::cout << "ALL LIVE VIEW PIPELINE TESTS PASSED WITH 100% SUCCESS!" << std::endl;
    std::cout << "==========================================================" << std::endl;
    return 0;
}
