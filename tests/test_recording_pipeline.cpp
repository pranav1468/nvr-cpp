#include "nvr/common/types.h"
#include "nvr/common/logger.h"
#include "nvr/common/time_utils.h"
#include "nvr/storage/database_manager.h"
#include "nvr/storage/segment_index.h"
#include "nvr/storage/retention_manager.h"
#include "nvr/media/stream_broker.h"
#include "nvr/recording/atomic_writer.h"
#include "nvr/recording/segmenter.h"
#include "nvr/ingress/rtp_depacketizer.h"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>

namespace {

void RunDatabaseTest() {
    std::cout << "[TEST 1] Running Database & SegmentIndex Tests..." << std::endl;
    std::string test_db = "./test_recordings/test_meta.db";
    std::filesystem::remove(test_db);

    bool init_ok = nvr::DatabaseManager::Instance().Initialize(test_db);
    assert(init_ok);
    assert(nvr::DatabaseManager::Instance().IsOpen());

    // Insert segment
    nvr::SegmentMetadata meta;
    meta.channel_id = 1;
    meta.file_path = "./test_recordings/cam1_20261007_100000.mp4";
    meta.start_time_ms = 1000000;
    meta.end_time_ms = 1060000;
    meta.duration_ms = 60000;
    meta.file_size_bytes = 2048576;
    meta.frame_count = 1500;
    meta.keyframe_count = 60;
    meta.codec = nvr::CodecType::H264;
    meta.width = 1920;
    meta.height = 1080;
    meta.fps = 25;

    bool insert_ok = nvr::SegmentIndex::Instance().InsertSegment(meta);
    assert(insert_ok);

    auto results = nvr::SegmentIndex::Instance().QuerySegments(1, 900000, 1100000);
    assert(results.size() == 1);
    assert(results[0].channel_id == 1);
    assert(results[0].frame_count == 1500);
    assert(results[0].file_size_bytes == 2048576);

    uint64_t total_bytes = nvr::SegmentIndex::Instance().GetTotalRecordingSizeBytes();
    assert(total_bytes == 2048576);

    int count = nvr::SegmentIndex::Instance().GetTotalSegmentCount();
    assert(count == 1);

    bool lock_ok = nvr::SegmentIndex::Instance().LockSegment(results[0].id, true);
    assert(lock_ok);

    auto unlocked = nvr::SegmentIndex::Instance().GetOldestUnlockedSegments(10);
    assert(unlocked.empty()); // Since it's locked

    nvr::SegmentIndex::Instance().LockSegment(results[0].id, false);
    unlocked = nvr::SegmentIndex::Instance().GetOldestUnlockedSegments(10);
    assert(unlocked.size() == 1);

    nvr::SegmentIndex::Instance().DeleteSegmentRecord(results[0].id);
    assert(nvr::SegmentIndex::Instance().GetTotalSegmentCount() == 0);

    std::cout << "  -> PASSED: SQLite WAL database & SegmentIndex verified." << std::endl;
}

void RunStreamBrokerTest() {
    std::cout << "[TEST 2] Running StreamBroker Pub/Sub Tests..." << std::endl;
    bool received = false;
    uint32_t rx_rtp_ts = 0;

    auto sub_id = nvr::StreamBroker::Instance().Subscribe(
        1, nvr::StreamType::MAIN,
        [&](const nvr::MediaPacketPtr& pkt) {
            received = true;
            rx_rtp_ts = pkt->rtp_timestamp;
        }
    );

    assert(nvr::StreamBroker::Instance().GetSubscriberCount(1, nvr::StreamType::MAIN) == 1);

    auto pkt = std::make_shared<nvr::MediaPacket>();
    pkt->channel_id = 1;
    pkt->stream_type = nvr::StreamType::MAIN;
    pkt->rtp_timestamp = 12345678;
    pkt->data = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E};

    nvr::StreamBroker::Instance().Publish(pkt);

    assert(received == true);
    assert(rx_rtp_ts == 12345678);

    nvr::StreamBroker::Instance().Unsubscribe(sub_id);
    assert(nvr::StreamBroker::Instance().GetSubscriberCount(1, nvr::StreamType::MAIN) == 0);

    std::cout << "  -> PASSED: In-memory StreamBroker pub/sub verified." << std::endl;
}

// Helper to construct a standard Annex-B H.264 1920x1080 SPS NAL unit
std::vector<uint8_t> MakeSps1080p() {
    // Baseline 1920x1080 SPS
    return {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x42, 0x00, 0x28, 0x8d, 0x8d, 0x40, 0x3c,
        0x01, 0x13, 0xf2, 0xcd, 0xc0, 0x40, 0x40, 0x50
    };
}

std::vector<uint8_t> MakePps() {
    return {
        0x00, 0x00, 0x00, 0x01,
        0x68, 0xce, 0x3c, 0x80
    };
}

std::vector<uint8_t> MakeIdrFrame() {
    return {
        0x00, 0x00, 0x00, 0x01,
        0x65, 0x88, 0x84, 0x00, 0x10, 0xff, 0xaa, 0xbb, 0xcc
    };
}

std::vector<uint8_t> MakeNonIdrFrame() {
    return {
        0x00, 0x00, 0x00, 0x01,
        0x41, 0x9a, 0x22, 0x11, 0x00, 0x55, 0x66, 0x77
    };
}

void RunAtomicWriterTest() {
    std::cout << "[TEST 3] Running Native MP4 Muxer & Atomic Writer Tests..." << std::endl;
    std::string test_dir = "./test_recordings";
    std::filesystem::create_directories(test_dir);

    nvr::AtomicWriter writer(1, test_dir);
    int64_t start_time = 1700000000000LL;
    bool ok = writer.StartSegment(start_time);
    assert(ok);
    assert(writer.IsActive());

    // 1. Write SPS packet
    auto sps_pkt = std::make_shared<nvr::MediaPacket>();
    sps_pkt->channel_id = 1;
    sps_pkt->codec = nvr::CodecType::H264;
    sps_pkt->is_keyframe = true;
    sps_pkt->data = MakeSps1080p();
    writer.WritePacket(sps_pkt);

    // 2. Write PPS packet
    auto pps_pkt = std::make_shared<nvr::MediaPacket>();
    pps_pkt->channel_id = 1;
    pps_pkt->codec = nvr::CodecType::H264;
    pps_pkt->is_keyframe = true;
    pps_pkt->data = MakePps();
    writer.WritePacket(pps_pkt);

    // 3. Write IDR Keyframe packet
    auto idr_pkt = std::make_shared<nvr::MediaPacket>();
    idr_pkt->channel_id = 1;
    idr_pkt->codec = nvr::CodecType::H264;
    idr_pkt->is_keyframe = true;
    idr_pkt->pts_us = 1000000;
    idr_pkt->data = MakeIdrFrame();
    writer.WritePacket(idr_pkt);

    // 4. Write several Non-IDR delta frames
    for (int i = 1; i <= 30; ++i) {
        auto delta_pkt = std::make_shared<nvr::MediaPacket>();
        delta_pkt->channel_id = 1;
        delta_pkt->codec = nvr::CodecType::H264;
        delta_pkt->is_keyframe = false;
        delta_pkt->pts_us = 1000000 + i * 40000; // 25 fps
        delta_pkt->data = MakeNonIdrFrame();
        writer.WritePacket(delta_pkt);
    }

    assert(writer.GetFrameCount() >= 30);
    assert(writer.GetCurrentBytesWritten() > 0);

    // 5. Finalize Segment
    nvr::SegmentMetadata meta;
    bool finalize_ok = writer.FinalizeSegment(meta);
    assert(finalize_ok);
    assert(!writer.IsActive());

    // Verify file existence on disk
    assert(std::filesystem::exists(meta.file_path));
    assert(meta.file_path.find(".mp4") != std::string::npos);
    assert(!std::filesystem::exists(meta.file_path.substr(0, meta.file_path.size() - 4) + ".tmp"));

    // Verify ISOBMFF header inside generated file
    std::ifstream mp4_file(meta.file_path, std::ios::binary);
    assert(mp4_file.is_open());
    char hdr[8];
    mp4_file.read(hdr, 8);
    assert(std::memcmp(hdr + 4, "ftyp", 4) == 0); // Must begin with ftyp box

    std::cout << "  -> PASSED: In-process MP4 container generated (" 
              << meta.file_size_bytes << " bytes, " << meta.frame_count 
              << " frames, file: " << meta.file_path << ")." << std::endl;
}

void RunSegmenterGopTest() {
    std::cout << "[TEST 4] Running GOP-Aligned Segmenter Rotation Tests..." << std::endl;
    std::string test_dir = "./test_recordings";
    // Target 2-second segment duration
    nvr::Segmenter segmenter(2, test_dir, 2);

    int64_t t0 = 1000000;

    // Send SPS & PPS
    auto sps = std::make_shared<nvr::MediaPacket>();
    sps->channel_id = 2;
    sps->codec = nvr::CodecType::H264;
    sps->is_keyframe = true;
    sps->wall_time_ms = t0;
    sps->data = MakeSps1080p();
    segmenter.PushPacket(sps);

    auto pps = std::make_shared<nvr::MediaPacket>();
    pps->channel_id = 2;
    pps->codec = nvr::CodecType::H264;
    pps->is_keyframe = true;
    pps->wall_time_ms = t0;
    pps->data = MakePps();
    segmenter.PushPacket(pps);

    // Send Keyframe at t = 0ms
    auto idr1 = std::make_shared<nvr::MediaPacket>();
    idr1->channel_id = 2;
    idr1->codec = nvr::CodecType::H264;
    idr1->is_keyframe = true;
    idr1->wall_time_ms = t0;
    idr1->pts_us = 0;
    idr1->data = MakeIdrFrame();
    segmenter.PushPacket(idr1);

    assert(segmenter.IsRecording());

    // Send delta frames past 2000ms threshold (t = 2100ms)
    // Segmenter should NOT rotate yet because this is not a keyframe!
    auto delta = std::make_shared<nvr::MediaPacket>();
    delta->channel_id = 2;
    delta->codec = nvr::CodecType::H264;
    delta->is_keyframe = false;
    delta->wall_time_ms = t0 + 2100;
    delta->pts_us = 2100000;
    delta->data = MakeNonIdrFrame();
    segmenter.PushPacket(delta);

    // Now send IDR Keyframe at t = 2200ms -> Triggers GOP-aligned rotation!
    auto idr2 = std::make_shared<nvr::MediaPacket>();
    idr2->channel_id = 2;
    idr2->codec = nvr::CodecType::H264;
    idr2->is_keyframe = true;
    idr2->wall_time_ms = t0 + 2200;
    idr2->pts_us = 2200000;
    idr2->data = MakeIdrFrame();
    segmenter.PushPacket(idr2);

    segmenter.FlushAndStop();

    // Verify that segments were committed to SQLite
    auto segs = nvr::SegmentIndex::Instance().QuerySegments(2, t0 - 1000, t0 + 5000);
    assert(segs.size() >= 2);
    std::cout << "  -> PASSED: GOP-aligned rotation verified (" << segs.size() << " segments indexed)." << std::endl;
}

void RunRtpDepacketizerTest() {
    std::cout << "[TEST 5] Running RFC 6184 RTP Depacketizer Tests..." << std::endl;
    bool received_frame = false;
    bool is_keyframe = false;
    std::vector<uint8_t> rx_data;

    nvr::RtpDepacketizer depack(
        1, nvr::StreamType::MAIN, nvr::CodecType::H264,
        [&](const nvr::MediaPacketPtr& pkt) {
            received_frame = true;
            is_keyframe = pkt->is_keyframe;
            rx_data = pkt->data;
        }
    );

    // Test 1: Single NAL Unit packet (SPS = type 7)
    uint8_t single_rtp[] = { 0x67, 0x42, 0x00, 0x1E };
    depack.ProcessRtpPacket(single_rtp, sizeof(single_rtp), 90000, 1, false);
    assert(received_frame);
    assert(rx_data.size() == 8); // 4-byte start code + 4 payload
    assert(rx_data[0] == 0 && rx_data[1] == 0 && rx_data[2] == 0 && rx_data[3] == 1);
    assert(rx_data[4] == 0x67);

    // Test 2: FU-A Fragmentation Unit (IDR = type 5, FU-A type 28)
    received_frame = false;
    // Packet 1: Start bit set (0x80 | 5 = 0x85)
    uint8_t fu1[] = { 0x7C, 0x85, 0xAA, 0xBB, 0xCC };
    depack.ProcessRtpPacket(fu1, sizeof(fu1), 180000, 2, false);
    assert(!received_frame); // Not yet complete

    // Packet 2: End bit set (0x40 | 5 = 0x45)
    uint8_t fu2[] = { 0x7C, 0x45, 0xDD, 0xEE, 0xFF };
    depack.ProcessRtpPacket(fu2, sizeof(fu2), 180000, 3, true);
    assert(received_frame);
    assert(is_keyframe == true);
    // Start code (4) + Reconstructed NAL Header (1) + Payload1 (3) + Payload2 (3) = 11 bytes
    assert(rx_data.size() == 11);
    assert(rx_data[4] == 0x65); // Reconstructed IDR header: (0x7C & 0xE0) | 5 = 0x60 | 5 = 0x65

    std::cout << "  -> PASSED: RFC 6184 FU-A reassembly & start code generation verified." << std::endl;
}

} // namespace

int main() {
    std::cout << "==========================================================" << std::endl;
    std::cout << "STARTING NVR RECORDING PIPELINE PRODUCTION TEST SUITE" << std::endl;
    std::cout << "==========================================================" << std::endl;

    try {
        std::filesystem::remove_all("./test_recordings");
        std::filesystem::create_directories("./test_recordings");

        RunDatabaseTest();
        RunStreamBrokerTest();
        RunAtomicWriterTest();
        RunSegmenterGopTest();
        RunRtpDepacketizerTest();

        std::filesystem::remove_all("./test_recordings");

        std::cout << "==========================================================" << std::endl;
        std::cout << "ALL PRODUCTION PIPELINE TESTS PASSED WITH 100% SUCCESS!" << std::endl;
        std::cout << "==========================================================" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "TEST SUITE FAILED WITH EXCEPTION: " << e.what() << std::endl;
        return 1;
    }
}
