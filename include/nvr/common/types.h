#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace nvr {

enum class CodecType {
    UNKNOWN = 0,
    H264,
    H265,
    AAC,
    PCMU,
    PCMA
};

inline const char* CodecToString(CodecType codec) {
    switch (codec) {
        case CodecType::H264: return "H264";
        case CodecType::H265: return "H265";
        case CodecType::AAC:  return "AAC";
        case CodecType::PCMU: return "PCMU";
        case CodecType::PCMA: return "PCMA";
        default:              return "UNKNOWN";
    }
}

inline CodecType StringToCodec(const std::string& str) {
    if (str == "H264") return CodecType::H264;
    if (str == "H265" || str == "HEVC") return CodecType::H265;
    if (str == "AAC") return CodecType::AAC;
    if (str == "PCMU") return CodecType::PCMU;
    if (str == "PCMA") return CodecType::PCMA;
    return CodecType::UNKNOWN;
}

enum class StreamType {
    MAIN = 0,
    SUB = 1
};

enum class NalType {
    UNKNOWN = 0,
    SLICE_NON_IDR,
    SLICE_IDR,
    SEI,
    SPS,
    PPS,
    VPS
};

enum class RecordMode {
    DISABLED = 0,
    CONTINUOUS = 1,
    MOTION_ONLY = 2,
    SCHEDULED = 3
};

enum class SessionState {
    DISCONNECTED = 0,
    CONNECTING,
    OPTIONS_SENT,
    DESCRIBE_SENT,
    SETUP_SENT,
    PLAYING,
    STREAMING,
    ERROR_BACKOFF
};

enum class MediaType {
    VIDEO = 0,
    AUDIO = 1
};

struct MediaPacket {
    int channel_id{0};
    StreamType stream_type{StreamType::MAIN};
    MediaType media_type{MediaType::VIDEO};
    CodecType codec{CodecType::H264};
    bool is_keyframe{false};
    uint32_t rtp_timestamp{0};
    int64_t pts_us{0};
    int64_t dts_us{0};
    int64_t wall_time_ms{0};
    uint16_t sequence_number{0};
    std::vector<uint8_t> data;

    bool IsAudio() const {
        return media_type == MediaType::AUDIO ||
               codec == CodecType::AAC ||
               codec == CodecType::PCMU ||
               codec == CodecType::PCMA;
    }
    bool IsVideo() const {
        return !IsAudio();
    }
};

using MediaPacketPtr = std::shared_ptr<MediaPacket>;

struct CameraConfig {
    int id{0};
    std::string name;
    std::string rtsp_url;
    std::string sub_rtsp_url;
    bool enabled{true};
    RecordMode record_mode{RecordMode::CONTINUOUS};
    int reconnect_delay_sec{3};
};

struct SegmentMetadata {
    int64_t id{0};
    int channel_id{0};
    std::string file_path;
    int64_t start_time_ms{0};
    int64_t end_time_ms{0};
    int64_t duration_ms{0};
    uint64_t file_size_bytes{0};
    uint32_t frame_count{0};
    uint32_t keyframe_count{0};
    bool is_locked{false};
    CodecType codec{CodecType::H264};
    uint32_t width{1920};
    uint32_t height{1080};
    uint32_t fps{25};
    bool has_audio{false};
    CodecType audio_codec{CodecType::UNKNOWN};
    uint32_t audio_sample_rate{8000};
    uint8_t audio_channels{1};
};

struct StorageConfig {
    std::string recording_path{"./recordings"};
    std::string database_path{"./nvr_metadata.db"};
    int segment_duration_seconds{60};
    int min_free_space_mb{2048};
    int max_retention_days{30};
};

struct SystemConfig {
    std::string device_name{"NVR_Core"};
    std::string log_level{"info"};
    int max_channels{8};
};

struct LiveConfig {
    int grid_layout{4};
    int max_queue_depth{2};
    bool drop_stale_frames{true};
};

struct AiConfig {
    bool enabled{false};
    int max_concurrent_models{2};
    double motion_threshold{0.05};
};

struct NvrConfig {
    SystemConfig system;
    StorageConfig storage;
    LiveConfig live;
    AiConfig ai;
    std::vector<CameraConfig> cameras;
};

} // namespace nvr
